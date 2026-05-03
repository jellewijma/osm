/**
 *  OSM - Android NDK compatibility header
 *
 *  Workaround for NDK libc++ make_shared incompatibility with QObject-derived types.
 *  The NDK's __shared_ptr_emplace doesn't properly convert to __shared_weak_count*
 *  when the managed type has virtual bases (like QObject).
 *
 *  This header provides osm_make_shared as a drop-in replacement that uses
 *  explicit shared_ptr construction instead of the problematic make_shared.
 */
#ifndef ANDROID_COMPAT_H
#define ANDROID_COMPAT_H

#ifdef Q_OS_ANDROID
#include <memory>

namespace std {

template<typename T, typename... Args>
inline shared_ptr<T> osm_make_shared(Args&&... args)
{
    return shared_ptr<T>(new T(std::forward<Args>(args)...));
}

} // namespace std

#endif // Q_OS_ANDROID

#endif // ANDROID_COMPAT_H
