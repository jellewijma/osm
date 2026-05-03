/**
 *  OSM
 *  Copyright (C) 2024  Pavel Smokotnin
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "opensles.h"
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <QCoreApplication>

namespace audio {

static bool slCheck(SLresult result, const char *msg)
{
    if (result != SL_RESULT_SUCCESS) {
        qCritical() << "OpenSL ES error:" << result << msg;
        return false;
    }
    return true;
}

// ─── OpenSLESPlugin ──────────────────────────────────────────────────────────

OpenSLESPlugin::OpenSLESPlugin() : m_list()
{
    SLresult result;

    // Create the engine
    result = slCreateEngine(&m_engineObject, 0, nullptr, 0, nullptr, nullptr);
    if (!slCheck(result, "slCreateEngine")) return;

    result = (*m_engineObject)->Realize(m_engineObject, SL_BOOLEAN_FALSE);
    if (!slCheck(result, "engine Realize")) return;

    result = (*m_engineObject)->GetInterface(m_engineObject, SL_IID_ENGINE, &m_engine);
    if (!slCheck(result, "engine GetInterface")) return;

    qDebug() << "OpenSL ES engine created successfully";
}

OpenSLESPlugin::~OpenSLESPlugin()
{
    if (m_engineObject) {
        (*m_engineObject)->Destroy(m_engineObject);
        m_engineObject = nullptr;
        m_engine = nullptr;
    }
}

QString OpenSLESPlugin::name() const
{
    return "OpenSLES";
}

DeviceInfo::List OpenSLESPlugin::getDeviceInfoList() const
{
    // On Android, OpenSL ES doesn't enumerate devices the same way desktop APIs do.
    // We expose a single "default" input and output device.
    DeviceInfo::List list;

    // Default input (microphone)
    DeviceInfo inputDevice("android_input_default", "OpenSLES");
    inputDevice.setName("Microphone");
    inputDevice.setDefaultSampleRate(48000);
    QStringList inputChannels;
    inputChannels << "1";
    inputDevice.setInputChannels(inputChannels);
    list << inputDevice;

    // Default output (speaker)
    DeviceInfo outputDevice("android_output_default", "OpenSLES");
    outputDevice.setName("Speaker");
    outputDevice.setDefaultSampleRate(48000);
    QStringList outputChannels;
    outputChannels << "1" << "2";
    outputDevice.setOutputChannels(outputChannels);
    list << outputDevice;

    m_list = list;
    return m_list;
}

DeviceInfo::Id OpenSLESPlugin::defaultDeviceId(const Plugin::Direction &mode) const
{
    switch (mode) {
    case Direction::Input:
        return "android_input_default";
    case Direction::Output:
        return "android_output_default";
    }
    return DeviceInfo::Id();
}

Format OpenSLESPlugin::deviceFormat(const DeviceInfo::Id &id, const Plugin::Direction &mode) const
{
    auto deviceIt = std::find_if(m_list.cbegin(), m_list.cend(), [&id](auto d) {
        return d.id() == id;
    });
    if (deviceIt == m_list.cend()) {
        return {1, 0};
    }
    auto device = *deviceIt;
    unsigned int count = (mode == Input ? device.inputChannels().count() : device.outputChannels().count());
    return {
        device.defaultSampleRate(),
        count
    };
}

Stream *OpenSLESPlugin::open(const DeviceInfo::Id &id, const Plugin::Direction &mode,
                              const Format &format, QIODevice *endpoint)
{
    std::lock_guard<std::mutex> lock(m_deviceListMutex);
    if (id.isNull()) {
        return nullptr;
    }

    OpenSLESDevice *device = m_devices[{mode, id}];
    if (!device) {
        device = new OpenSLESDevice(m_engine, id, mode, format, m_deviceListMutex);
        connect(device, &OpenSLESDevice::closed, this, [this, mode, id, device]() {
            m_devices[{mode, id}] = nullptr;
            device->deleteLater();
            m_deviceListMutex.unlock();
        }, Qt::DirectConnection);

        m_devices[{mode, id}] = device;
    }

    if (!device->start()) {
        return nullptr;
    }
    device->setKeepAlive();

    auto stream = new Stream(device->format());
    endpoint->open(mode == Input ? QIODevice::WriteOnly : QIODevice::ReadOnly);

    connect(stream, &Stream::closeMe, device, [endpoint, stream, device]() {
        if (endpoint->isOpen()) {
            endpoint->close();
        }
        device->removeCallback(stream);
        usleep(30'000); // 30ms
        stream->deleteLater();
    }, Qt::DirectConnection);

    OpenSLESDevice::Callback endpointCallback;
    switch (mode) {
    case Input:
        endpointCallback = [endpoint](float *buffer, size_t size) {
            if (endpoint->isWritable()) {
                endpoint->write(reinterpret_cast<char *>(buffer), size * sizeof(float));
            }
        };
        break;
    case Output:
        endpointCallback = [endpoint](float *buffer, size_t size) {
            if (endpoint->isReadable()) {
                endpoint->read(reinterpret_cast<char *>(buffer), size * sizeof(float));
            }
        };
        break;
    }

    // Register callback directly (thread-safe via internal mutex)
    device->addCallback(stream, endpointCallback);

    qDebug() << "OpenSL ES stream opened for" << (mode == Plugin::Input ? "input" : "output")
             << "id:" << id << "sampleRate:" << format.sampleRate << "channels:" << format.channelCount;

    while (!device->active()) {
        usleep(10'000);
    }
    return stream;
}

// ─── OpenSLESDevice ──────────────────────────────────────────────────────────

OpenSLESDevice::OpenSLESDevice(SLEngineItf engine, const DeviceInfo::Id &id,
                               const Plugin::Direction &mode, const Format &format,
                               std::mutex &mutex)
    : QObject(), m_engine(engine), m_callbacks(),
      m_id(id), m_mode(mode), m_format(format),
      m_mutex(mutex), m_keepAlive(false), m_active(false)
{
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        m_buffers[i].resize(BUFFER_SIZE * format.channelCount);
    }
    m_floatBuffer.resize(BUFFER_SIZE * format.channelCount);
}

OpenSLESDevice::~OpenSLESDevice()
{
    stop();
}

bool OpenSLESDevice::start()
{
    if (m_active) {
        return true;
    }

    SLresult result;

    if (m_mode == Plugin::Input) {
        // Configure audio source (mic)
        SLDataLocator_IODevice locDevice = {
            SL_DATALOCATOR_IODEVICE,
            SL_IODEVICE_AUDIOINPUT,
            SL_DEFAULTDEVICEID_AUDIOINPUT,
            nullptr
        };
        SLDataSource audioSrc = {&locDevice, nullptr};

        // Configure audio sink (buffer queue)
        SLDataLocator_AndroidSimpleBufferQueue locBQ = {
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
            NUM_BUFFERS
        };
        SLDataFormat_PCM formatPCM = {
            SL_DATAFORMAT_PCM,
            static_cast<SLuint32>(m_format.channelCount),
            static_cast<SLuint32>(m_format.sampleRate * 1000), // milliHz
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            (m_format.channelCount == 1) ? SL_SPEAKER_FRONT_CENTER :
                (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
            SL_BYTEORDER_LITTLEENDIAN
        };
        SLDataSink audioSnk = {&locBQ, &formatPCM};

        // Create audio recorder
        const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION};
        const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
        result = (*m_engine)->CreateAudioRecorder(m_engine, &m_recorderObject,
                                                   &audioSrc, &audioSnk, 2, ids, req);
        if (!slCheck(result, "CreateAudioRecorder")) return false;

        // Try to set performance mode for low latency
        SLAndroidConfigurationItf configItf;
        if ((*m_recorderObject)->GetInterface(m_recorderObject, SL_IID_ANDROIDCONFIGURATION,
                                               &configItf) == SL_RESULT_SUCCESS) {
            SLuint32 presetValue = SL_ANDROID_RECORDING_PRESET_UNPROCESSED;
            (*configItf)->SetConfiguration(configItf, SL_ANDROID_KEY_RECORDING_PRESET,
                                           &presetValue, sizeof(SLuint32));
        }

        result = (*m_recorderObject)->Realize(m_recorderObject, SL_BOOLEAN_FALSE);
        if (!slCheck(result, "recorder Realize")) return false;

        // Get buffer queue interface
        result = (*m_recorderObject)->GetInterface(m_recorderObject,
                                                    SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                    &m_bufferQueue);
        if (!slCheck(result, "recorder GetInterface BQ")) return false;

        // Register callback
        result = (*m_bufferQueue)->RegisterCallback(m_bufferQueue, inputCallback, this);
        if (!slCheck(result, "recorder RegisterCallback")) return false;

        // Enqueue initial buffers
        for (int i = 0; i < NUM_BUFFERS; ++i) {
            result = (*m_bufferQueue)->Enqueue(m_bufferQueue, m_buffers[i].data(),
                                                m_buffers[i].size() * sizeof(int16_t));
            if (!slCheck(result, "recorder initial Enqueue")) return false;
        }

        // Start recording
        SLRecordItf recordItf;
        result = (*m_recorderObject)->GetInterface(m_recorderObject, SL_IID_RECORD, &recordItf);
        if (!slCheck(result, "recorder GetInterface Record")) return false;

        result = (*recordItf)->SetRecordState(recordItf, SL_RECORDSTATE_RECORDING);
        if (!slCheck(result, "SetRecordState")) return false;

    } else {
        // Output mode
        // Create output mix
        result = (*m_engine)->CreateOutputMix(m_engine, &m_outputMixObject, 0, nullptr, nullptr);
        if (!slCheck(result, "CreateOutputMix")) return false;
        result = (*m_outputMixObject)->Realize(m_outputMixObject, SL_BOOLEAN_FALSE);
        if (!slCheck(result, "outputMix Realize")) return false;

        // Configure audio source (buffer queue)
        SLDataLocator_AndroidSimpleBufferQueue locBQ = {
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
            NUM_BUFFERS
        };
        SLDataFormat_PCM formatPCM = {
            SL_DATAFORMAT_PCM,
            static_cast<SLuint32>(m_format.channelCount),
            static_cast<SLuint32>(m_format.sampleRate * 1000), // milliHz
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            (m_format.channelCount == 2) ?
                (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT) : SL_SPEAKER_FRONT_CENTER,
            SL_BYTEORDER_LITTLEENDIAN
        };
        SLDataSource audioSrc = {&locBQ, &formatPCM};

        // Configure audio sink (output mix)
        SLDataLocator_OutputMix locOutMix = {
            SL_DATALOCATOR_OUTPUTMIX,
            m_outputMixObject
        };
        SLDataSink audioSnk = {&locOutMix, nullptr};

        // Create player
        const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
        const SLboolean req[] = {SL_BOOLEAN_TRUE};
        result = (*m_engine)->CreateAudioPlayer(m_engine, &m_playerObject,
                                                 &audioSrc, &audioSnk, 1, ids, req);
        if (!slCheck(result, "CreateAudioPlayer")) return false;

        result = (*m_playerObject)->Realize(m_playerObject, SL_BOOLEAN_FALSE);
        if (!slCheck(result, "player Realize")) return false;

        // Get buffer queue interface
        result = (*m_playerObject)->GetInterface(m_playerObject,
                                                  SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                  &m_bufferQueue);
        if (!slCheck(result, "player GetInterface BQ")) return false;

        // Register callback
        result = (*m_bufferQueue)->RegisterCallback(m_bufferQueue, outputCallback, this);
        if (!slCheck(result, "player RegisterCallback")) return false;

        // Enqueue initial silent buffers
        for (int i = 0; i < NUM_BUFFERS; ++i) {
            std::memset(m_buffers[i].data(), 0, m_buffers[i].size() * sizeof(int16_t));
            result = (*m_bufferQueue)->Enqueue(m_bufferQueue, m_buffers[i].data(),
                                                m_buffers[i].size() * sizeof(int16_t));
            if (!slCheck(result, "player initial Enqueue")) return false;
        }

        // Start playback
        SLPlayItf playItf;
        result = (*m_playerObject)->GetInterface(m_playerObject, SL_IID_PLAY, &playItf);
        if (!slCheck(result, "player GetInterface Play")) return false;

        result = (*playItf)->SetPlayState(playItf, SL_PLAYSTATE_PLAYING);
        if (!slCheck(result, "SetPlayState")) return false;
    }

    m_active = true;
    qDebug() << "OpenSL ES device started:" << m_id
             << "mode:" << (m_mode == Plugin::Input ? "input" : "output")
             << "sampleRate:" << m_format.sampleRate
             << "channels:" << m_format.channelCount;
    return true;
}

void OpenSLESDevice::stop()
{
    m_active = false;

    if (m_recorderObject) {
        SLRecordItf recordItf;
        if ((*m_recorderObject)->GetInterface(m_recorderObject, SL_IID_RECORD, &recordItf)
            == SL_RESULT_SUCCESS) {
            (*recordItf)->SetRecordState(recordItf, SL_RECORDSTATE_STOPPED);
        }
        (*m_recorderObject)->Destroy(m_recorderObject);
        m_recorderObject = nullptr;
    }

    if (m_playerObject) {
        SLPlayItf playItf;
        if ((*m_playerObject)->GetInterface(m_playerObject, SL_IID_PLAY, &playItf)
            == SL_RESULT_SUCCESS) {
            (*playItf)->SetPlayState(playItf, SL_PLAYSTATE_STOPPED);
        }
        (*m_playerObject)->Destroy(m_playerObject);
        m_playerObject = nullptr;
    }

    if (m_outputMixObject) {
        (*m_outputMixObject)->Destroy(m_outputMixObject);
        m_outputMixObject = nullptr;
    }

    m_bufferQueue = nullptr;
    emit closed();
}

Format OpenSLESDevice::format() const
{
    return m_format;
}

bool OpenSLESDevice::active() const
{
    return m_active;
}

void OpenSLESDevice::setKeepAlive()
{
    m_keepAlive = true;
}

void OpenSLESDevice::addCallback(Stream *stream, Callback callback)
{
    std::lock_guard<std::mutex> guard(m_callbacksMutex);
    m_callbacks[stream] = callback;
    qDebug() << "OpenSL ES: callback registered, total callbacks:" << m_callbacks.size();
}

void OpenSLESDevice::removeCallback(Stream *stream)
{
    std::lock_guard<std::mutex> guard(m_callbacksMutex);
    m_callbacks.remove(stream);
    qDebug() << "OpenSL ES: callback removed, remaining:" << m_callbacks.size();
}

void OpenSLESDevice::inputCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    auto *device = static_cast<OpenSLESDevice *>(context);
    device->processInput();
}

void OpenSLESDevice::outputCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    auto *device = static_cast<OpenSLESDevice *>(context);
    device->processOutput();
}

void OpenSLESDevice::processInput()
{
    if (!m_active || !m_bufferQueue) return;

    // Convert int16 buffer to float
    auto &currentBuf = m_buffers[m_currentBuffer];
    size_t sampleCount = currentBuf.size();
    m_floatBuffer.resize(sampleCount);

    for (size_t i = 0; i < sampleCount; ++i) {
        m_floatBuffer[i] = static_cast<float>(currentBuf[i]) / 32768.0f;
    }

    // Dispatch to callbacks under lock
    {
        std::lock_guard<std::mutex> guard(m_callbacksMutex);
        for (auto &callback : m_callbacks) {
            callback(m_floatBuffer.data(), m_floatBuffer.size());
        }
    }

    // Re-enqueue the buffer for the next recording cycle
    (*m_bufferQueue)->Enqueue(m_bufferQueue, currentBuf.data(),
                              currentBuf.size() * sizeof(int16_t));

    m_currentBuffer = (m_currentBuffer + 1) % NUM_BUFFERS;
}

void OpenSLESDevice::processOutput()
{
    if (!m_active || !m_bufferQueue) return;

    auto &currentBuf = m_buffers[m_currentBuffer];
    size_t sampleCount = currentBuf.size();
    m_floatBuffer.resize(sampleCount);

    // Clear buffer
    std::memset(m_floatBuffer.data(), 0, sampleCount * sizeof(float));

    // Let callbacks fill the float buffer
    {
        std::lock_guard<std::mutex> guard(m_callbacksMutex);
        for (auto &callback : m_callbacks) {
            callback(m_floatBuffer.data(), m_floatBuffer.size());
        }
    }

    // Convert float to int16
    for (size_t i = 0; i < sampleCount; ++i) {
        float sample = m_floatBuffer[i];
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        currentBuf[i] = static_cast<int16_t>(sample * 32767.0f);
    }

    // Enqueue the filled buffer
    (*m_bufferQueue)->Enqueue(m_bufferQueue, currentBuf.data(),
                              currentBuf.size() * sizeof(int16_t));

    m_currentBuffer = (m_currentBuffer + 1) % NUM_BUFFERS;
}

} // namespace audio
