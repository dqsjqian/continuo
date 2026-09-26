#pragma once

// Mira/core/platform.hpp — the native handle, and what each platform can do.
//
// Mira targets every platform its hosts ship on: macOS, Linux, Windows,
// iOS, and Android. Those platforms do not agree on what an asynchronous I/O
// API even *is*, and that disagreement is the single most important
// architectural fact in this library:
//
//   * POSIX (kqueue, epoll) is a **reactor**: it tells you a descriptor is
//     *ready*, and you then perform the read yourself.
//   * Windows (IOCP) is a **proactor**: you *submit* a read, and it tells you
//     when that read has *completed*.
//
// A library whose public API is readiness-shaped cannot be implemented on
// IOCP without emulating it badly. A completion-shaped API, by contrast, maps
// onto IOCP directly and is trivially emulated on a reactor (wait for ready,
// then read). So Mira's public I/O API is completion-shaped on every
// platform — the same conclusion asio reached, and the direction io_uring has
// since taken Linux.
//
// Readiness therefore stays an *implementation detail* of the POSIX backends,
// exposed only as a clearly-marked extension for embedding third-party
// descriptors.

#include <cstddef>

#if defined(_WIN32)
    #define MIRA_PLATFORM_WINDOWS 1
    #define MIRA_IO_BACKEND_IOCP 1
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #define MIRA_PLATFORM_APPLE 1
    #define MIRA_IO_BACKEND_KQUEUE 1
    #if TARGET_OS_IPHONE
        #define MIRA_PLATFORM_IOS 1
    #else
        #define MIRA_PLATFORM_MACOS 1
    #endif
#elif defined(__ANDROID__)
    #define MIRA_PLATFORM_ANDROID 1
    #define MIRA_IO_BACKEND_EPOLL 1
#elif defined(__linux__)
    #define MIRA_PLATFORM_LINUX 1
    #define MIRA_IO_BACKEND_EPOLL 1
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #define MIRA_PLATFORM_BSD 1
    #define MIRA_IO_BACKEND_KQUEUE 1
#else
    #error "Mira: unsupported platform — add a readiness or completion backend"
#endif

/// True where the loop can report raw descriptor readiness.
///
/// Available on every POSIX backend; absent on IOCP, which reports completed
/// operations instead. Code inside `#if MIRA_HAS_READINESS_API` is by
/// definition not portable — the completion API is.
#if defined(MIRA_IO_BACKEND_KQUEUE) || defined(MIRA_IO_BACKEND_EPOLL)
    #define MIRA_HAS_READINESS_API 1
#else
    #define MIRA_HAS_READINESS_API 0
#endif

#if MIRA_PLATFORM_WINDOWS
    #include <basetsd.h>
#endif

namespace Mira {

#if MIRA_PLATFORM_WINDOWS
/// A socket or file handle the loop can perform I/O on.
///
/// Spelled as the integer type behind `SOCKET` so that this header does not
/// drag `<winsock2.h>` into every translation unit that includes it.
using NativeHandle = UINT_PTR;

/// Value representing "no handle" (`INVALID_SOCKET`). The signed `-1`
/// converts modulo 2^N, matching the Win32 `(UINT_PTR)-1` idiom; a `~0ull`
/// source would make the cast a no-op on toolchains where UINT_PTR is
/// already `unsigned long long` (MinGW), tripping -Wuseless-cast.
inline constexpr NativeHandle invalid_handle = static_cast<NativeHandle>(-1);
#else
/// A file descriptor the loop can perform I/O on.
using NativeHandle = int;

/// Value representing "no handle".
inline constexpr NativeHandle invalid_handle = -1;
#endif

/// Which asynchronous I/O mechanism this build is using.
enum class IoBackend {
    kqueue,
    epoll,
    iocp,
};

/// The backend compiled into this build — useful in diagnostics and tests.
[[nodiscard]] constexpr IoBackend io_backend() noexcept {
#if defined(MIRA_IO_BACKEND_IOCP)
    return IoBackend::iocp;
#elif defined(MIRA_IO_BACKEND_KQUEUE)
    return IoBackend::kqueue;
#else
    return IoBackend::epoll;
#endif
}

/// Human-readable backend name.
[[nodiscard]] constexpr const char* io_backend_name() noexcept {
    switch (io_backend()) {
    case IoBackend::iocp:
        return "iocp";
    case IoBackend::kqueue:
        return "kqueue";
    case IoBackend::epoll:
        return "epoll";
    }
    return "unknown";
}

}  // namespace Mira
