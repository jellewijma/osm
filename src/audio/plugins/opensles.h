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
#ifndef AUDIO_OPENSLESPLUGIN_H
#define AUDIO_OPENSLESPLUGIN_H

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

#include "../plugin.h"
#include <QThread>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

namespace audio {

class OpenSLESDevice;

class OpenSLESPlugin : public Plugin
{
    Q_OBJECT

public:
    OpenSLESPlugin();
    ~OpenSLESPlugin();

    QString name() const override;
    DeviceInfo::List getDeviceInfoList() const override;
    DeviceInfo::Id defaultDeviceId(const Direction &mode) const override;

    Format deviceFormat(const DeviceInfo::Id &id, const Direction &mode) const override;
    Stream *open(const DeviceInfo::Id &id, const Direction &mode, const Format &format, QIODevice *endpoint) override;

private:
    SLObjectItf m_engineObject = nullptr;
    SLEngineItf m_engine = nullptr;

    mutable DeviceInfo::List m_list;
    QHash<std::pair<Direction, DeviceInfo::Id>, OpenSLESDevice *> m_devices;
    std::mutex m_deviceListMutex;
};

class OpenSLESDevice : public QObject
{
    Q_OBJECT

public:
    using Callback = std::function<void(float *, size_t)>;

    OpenSLESDevice(SLEngineItf engine, const DeviceInfo::Id &id,
                   const Plugin::Direction &mode, const Format &format,
                   std::mutex &mutex);
    ~OpenSLESDevice();

    bool start();
    void stop();

    Format format() const;
    bool active() const;
    void setKeepAlive();

    // Thread-safe callback management (called directly, NOT via queued connection)
    void addCallback(Stream *stream, Callback callback);
    void removeCallback(Stream *stream);

signals:
    void closed();

private:
    static void inputCallback(SLAndroidSimpleBufferQueueItf bq, void *context);
    static void outputCallback(SLAndroidSimpleBufferQueueItf bq, void *context);
    void processInput();
    void processOutput();

    SLEngineItf m_engine;
    SLObjectItf m_outputMixObject = nullptr;
    SLObjectItf m_playerObject = nullptr;
    SLObjectItf m_recorderObject = nullptr;
    SLAndroidSimpleBufferQueueItf m_bufferQueue = nullptr;

    QHash<Stream *, Callback> m_callbacks;
    std::mutex m_callbacksMutex;  // protects m_callbacks from audio thread vs main thread

    static constexpr int NUM_BUFFERS = 2;
    static constexpr int BUFFER_SIZE = 1024;
    std::vector<int16_t> m_buffers[NUM_BUFFERS];
    std::vector<float> m_floatBuffer;
    int m_currentBuffer = 0;

    DeviceInfo::Id m_id;
    Plugin::Direction m_mode;
    Format m_format;
    std::mutex &m_mutex;
    std::atomic<bool> m_keepAlive, m_active;
};

} // namespace audio

Q_DECLARE_METATYPE(audio::OpenSLESDevice::Callback)

#endif // AUDIO_OPENSLESPLUGIN_H
