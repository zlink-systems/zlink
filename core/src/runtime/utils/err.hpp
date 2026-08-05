/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ERR_HPP_INCLUDED__
#define __ZLINK_ERR_HPP_INCLUDED__

#include <assert.h>
#if defined _WIN32_WCE
#include "..\builds\msvc\errno.hpp"
#else
#include <errno.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef ZLINK_HAVE_WINDOWS
#include <netdb.h>
#endif

#include "utils/likely.hpp"

// Public extended errno values are defined in zlink_errno.h and internal
// classification helpers live in core/internal_errno.hpp.

namespace zlink
{
const char *errno_to_string (int errno_);
#if defined __clang__
#if __has_feature(attribute_analyzer_noreturn)
void zlink_abort (const char *errmsg_) __attribute__ ((analyzer_noreturn));
#else
void zlink_abort (const char *errmsg_);
#endif
#elif defined __MSCVER__
__declspec (noreturn) void zlink_abort (const char *errmsg_);
#else
void zlink_abort (const char *errmsg_);
#endif
void print_backtrace ();
}

#ifdef ZLINK_HAVE_WINDOWS

namespace zlink
{
const char *wsa_error ();
const char *wsa_error_no (int no_, const char *wsae_wouldblock_string_ = "Operation would block");
void win_error (char *buffer_, size_t buffer_size_);
int wsa_error_to_errno (int errcode_);
}

//  Provides convenient way to check WSA-style errors on Windows.
#define wsa_assert(x)                                                                              \
    do {                                                                                           \
        if (unlikely (!(x))) {                                                                     \
            const char *errstr = zlink::wsa_error ();                                              \
            if (errstr != NULL) {                                                                  \
                fprintf (stderr, "Assertion failed: %s [%i] (%s:%d)\n", errstr,                    \
                         WSAGetLastError (), __FILE__, __LINE__);                                  \
                fflush (stderr);                                                                   \
                zlink::zlink_abort (errstr);                                                       \
            }                                                                                      \
        }                                                                                          \
    } while (false)

//  Provides convenient way to assert on WSA-style errors on Windows.
#define wsa_assert_no(no)                                                                          \
    do {                                                                                           \
        const char *errstr = zlink::wsa_error_no (no);                                             \
        if (errstr != NULL) {                                                                      \
            fprintf (stderr, "Assertion failed: %s (%s:%d)\n", errstr, __FILE__, __LINE__);        \
            fflush (stderr);                                                                       \
            zlink::zlink_abort (errstr);                                                           \
        }                                                                                          \
    } while (false)

// Provides convenient way to check GetLastError-style errors on Windows.
#define win_assert(x)                                                                              \
    do {                                                                                           \
        if (unlikely (!(x))) {                                                                     \
            char errstr[256];                                                                      \
            zlink::win_error (errstr, 256);                                                        \
            fprintf (stderr, "Assertion failed: %s (%s:%d)\n", errstr, __FILE__, __LINE__);        \
            fflush (stderr);                                                                       \
            zlink::zlink_abort (errstr);                                                           \
        }                                                                                          \
    } while (false)

#endif

//  This macro works in exactly the same way as the normal assert. It is used
//  in its stead because standard assert on Win32 in broken - it prints nothing
//  when used within the scope of JNI library.
#define zlink_assert(x)                                                                            \
    do {                                                                                           \
        if (unlikely (!(x))) {                                                                     \
            fprintf (stderr, "Assertion failed: %s (%s:%d)\n", #x, __FILE__, __LINE__);            \
            fflush (stderr);                                                                       \
            zlink::zlink_abort (#x);                                                               \
        }                                                                                          \
    } while (false)

//  Provides convenient way to check for errno-style errors.
#define errno_assert(x)                                                                            \
    do {                                                                                           \
        if (unlikely (!(x))) {                                                                     \
            const char *errstr = strerror (errno);                                                 \
            fprintf (stderr, "%s (%s:%d)\n", errstr, __FILE__, __LINE__);                          \
            fflush (stderr);                                                                       \
            zlink::zlink_abort (errstr);                                                           \
        }                                                                                          \
    } while (false)

//  Provides convenient way to check for POSIX errors.
#define posix_assert(x)                                                                            \
    do {                                                                                           \
        if (unlikely (x)) {                                                                        \
            const char *errstr = strerror (x);                                                     \
            fprintf (stderr, "%s (%s:%d)\n", errstr, __FILE__, __LINE__);                          \
            fflush (stderr);                                                                       \
            zlink::zlink_abort (errstr);                                                           \
        }                                                                                          \
    } while (false)

//  Provides convenient way to check for errors from getaddrinfo.
#define gai_assert(x)                                                                              \
    do {                                                                                           \
        if (unlikely (x)) {                                                                        \
            const char *errstr = gai_strerror (x);                                                 \
            fprintf (stderr, "%s (%s:%d)\n", errstr, __FILE__, __LINE__);                          \
            fflush (stderr);                                                                       \
            zlink::zlink_abort (errstr);                                                           \
        }                                                                                          \
    } while (false)

//  Provides convenient way to check whether memory allocation have succeeded.
#define alloc_assert(x)                                                                            \
    do {                                                                                           \
        if (unlikely (!(x))) {                                                                     \
            fprintf (stderr, "FATAL ERROR: OUT OF MEMORY (%s:%d)\n", __FILE__, __LINE__);          \
            fflush (stderr);                                                                       \
            zlink::zlink_abort ("FATAL ERROR: OUT OF MEMORY");                                     \
        }                                                                                          \
    } while (false)

#endif
