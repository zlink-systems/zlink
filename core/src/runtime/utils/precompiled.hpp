/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_PRECOMPILED_HPP_INCLUDED__
#define __ZLINK_PRECOMPILED_HPP_INCLUDED__

//  On AIX platform, poll.h has to be included first to get consistent
//  definition of pollfd structure (AIX uses 'reqevents' and 'retnevents'
//  instead of 'events' and 'revents' and defines macros to map from POSIX-y
//  names to AIX-specific names).
//  zlink.h must be included *after* poll.h for AIX to build properly.
//  precompiled.hpp includes include/zlink.h
#if defined ZLINK_POLL_BASED_ON_POLL && defined ZLINK_HAVE_AIX
#include <poll.h>
#endif

#include "platform.hpp"

#define __STDC_LIMIT_MACROS

// This must be included before any windows headers are compiled.
#if defined ZLINK_HAVE_WINDOWS
#include "utils/windows.hpp"
#endif

#if defined ZLINK_HAVE_OPENBSD
#define ucred sockpeercred
#endif

// 0MQ definitions and exported functions
#include "../include/zlink.h"
#include "core/internal_errno.hpp"
#include "core/internal_defs.hpp"

#ifndef ZLINK_INTERNAL_EXPORT
#if defined _WIN32 && defined DLL_EXPORT && !defined ZLINK_STATIC
#define ZLINK_INTERNAL_EXPORT __declspec (dllexport)
#else
#define ZLINK_INTERNAL_EXPORT
#endif
#endif

extern "C" {
ZLINK_INTERNAL_EXPORT int zlink_stream_attach_raw (
  void *s_, int (*on_raw_) (const zlink_routing_id_t *, zlink_msg_t *));
ZLINK_INTERNAL_EXPORT int zlink_stream_detach (void *s_);
}

#ifndef ZLINK_SNDMORE
#define ZLINK_SNDMORE ((zlink_send_flags_t) 0x0002u)
#endif
#ifndef ZLINK_INTERNAL_OPT_RCVTIMEO
#define ZLINK_INTERNAL_OPT_RCVTIMEO 27
#endif
#ifndef ZLINK_POLLIN
#define ZLINK_POLLIN 1
#endif
#ifndef ZLINK_POLLOUT
#define ZLINK_POLLOUT 2
#endif
#ifndef ZLINK_POLLERR
#define ZLINK_POLLERR 4
#endif
#ifndef ZLINK_POLLPRI
#define ZLINK_POLLPRI 8
#endif
#ifndef ZLINK_POLLITEMS_DFLT
#define ZLINK_POLLITEMS_DFLT 16
#endif
#ifndef ZLINK_HAVE_POLLER
#define ZLINK_HAVE_POLLER 1
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN ENOTCONN
#endif

// Precompiled headers are currently enabled only on MSVC builds.
#ifdef _MSC_VER

// standard C headers
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <io.h>
#include <ipexport.h>
#include <iphlpapi.h>
#include <limits.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <process.h>
#include <rpc.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>

// standard C++ headers
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if _MSC_VER >= 1800
#include <inttypes.h>
#endif

#if _MSC_VER >= 1700
#include <atomic>
#endif

#if defined _WIN32_WCE
#include <cmnintrin.h>
#else
#include <intrin.h>
#endif

#include "core/options.hpp"

#endif // _MSC_VER

#endif //ifndef __ZLINK_PRECOMPILED_HPP_INCLUDED__
