/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_DEBUG_HPP_INCLUDED__
#define __ZLINK_ASIO_DEBUG_HPP_INCLUDED__

#include <cstdio>

//  Unified debug macros for ASIO components
//  Enable with -DZLINK_ASIO_DEBUG=1 during compilation
//
//  Usage:
//    ASIO_DBG_ENGINE("read completed: %zu bytes", bytes);
//    ASIO_DBG_POLLER("timer fired");
//    ASIO_DBG_CONN("connecting to %s", endpoint);

#ifdef ZLINK_ASIO_DEBUG

#define ASIO_DBG(category, ...)                                                                    \
    do {                                                                                           \
        fprintf (stderr, "[ASIO:" category "] ");                                                  \
        fprintf (stderr, __VA_ARGS__);                                                             \
        fputc ('\n', stderr);                                                                      \
    } while (0)

#define ASIO_DBG_THIS(category, ...)                                                               \
    do {                                                                                           \
        fprintf (stderr, "[ASIO:" category ":%p] ", static_cast<void *> (this));                   \
        fprintf (stderr, __VA_ARGS__);                                                             \
        fputc ('\n', stderr);                                                                      \
    } while (0)

#else

#define ASIO_DBG(category, ...) ((void) 0)
#define ASIO_DBG_THIS(category, ...) ((void) 0)

#endif

//  Component-specific macros
#define ASIO_DBG_ENGINE(...) ASIO_DBG_THIS ("ENGINE", __VA_ARGS__)
#define ASIO_DBG_ZMP(...) ASIO_DBG_THIS ("ZMP", __VA_ARGS__)
#define ASIO_DBG_POLLER(...) ASIO_DBG_THIS ("POLLER", __VA_ARGS__)
#define ASIO_DBG_CONN(...) ASIO_DBG_THIS ("CONN", __VA_ARGS__)
#define ASIO_DBG_LISTENER(...) ASIO_DBG_THIS ("LISTENER", __VA_ARGS__)

//  Severity-based macros (with this pointer)
#define ASIO_LOG_ERROR(...) ASIO_DBG_THIS ("ERROR", __VA_ARGS__)
#define ASIO_LOG_WARN(...) ASIO_DBG_THIS ("WARN", __VA_ARGS__)
#define ASIO_LOG_INFO(...) ASIO_DBG_THIS ("INFO", __VA_ARGS__)
#define ASIO_LOG_DEBUG(...) ASIO_DBG_THIS ("DEBUG", __VA_ARGS__)

//  Global severity macros (without this pointer)
#define ASIO_GLOBAL_ERROR(...) ASIO_DBG ("ERROR", __VA_ARGS__)
#define ASIO_GLOBAL_WARN(...) ASIO_DBG ("WARN", __VA_ARGS__)
#define ASIO_GLOBAL_INFO(...) ASIO_DBG ("INFO", __VA_ARGS__)
#define ASIO_GLOBAL_DEBUG(...) ASIO_DBG ("DEBUG", __VA_ARGS__)

#endif
