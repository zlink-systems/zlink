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

#define ASIO_DBG(category, fmt, ...)                                                               \
    do {                                                                                           \
        fprintf (stderr, "[ASIO:" category "] " fmt "\n", ##__VA_ARGS__);                          \
    } while (0)

#define ASIO_DBG_THIS(category, fmt, ...)                                                          \
    do {                                                                                           \
        fprintf (stderr, "[ASIO:" category ":%p] " fmt "\n", static_cast<void *> (this),           \
                 ##__VA_ARGS__);                                                                   \
    } while (0)

#else

#define ASIO_DBG(category, fmt, ...) ((void) 0)
#define ASIO_DBG_THIS(category, fmt, ...) ((void) 0)

#endif

//  Component-specific macros
#define ASIO_DBG_ENGINE(fmt, ...) ASIO_DBG_THIS ("ENGINE", fmt, ##__VA_ARGS__)
#define ASIO_DBG_ZMP(fmt, ...) ASIO_DBG_THIS ("ZMP", fmt, ##__VA_ARGS__)
#define ASIO_DBG_POLLER(fmt, ...) ASIO_DBG_THIS ("POLLER", fmt, ##__VA_ARGS__)
#define ASIO_DBG_CONN(fmt, ...) ASIO_DBG_THIS ("CONN", fmt, ##__VA_ARGS__)
#define ASIO_DBG_LISTENER(fmt, ...) ASIO_DBG_THIS ("LISTENER", fmt, ##__VA_ARGS__)

//  Severity-based macros (with this pointer)
#define ASIO_LOG_ERROR(fmt, ...) ASIO_DBG_THIS ("ERROR", fmt, ##__VA_ARGS__)
#define ASIO_LOG_WARN(fmt, ...) ASIO_DBG_THIS ("WARN", fmt, ##__VA_ARGS__)
#define ASIO_LOG_INFO(fmt, ...) ASIO_DBG_THIS ("INFO", fmt, ##__VA_ARGS__)
#define ASIO_LOG_DEBUG(fmt, ...) ASIO_DBG_THIS ("DEBUG", fmt, ##__VA_ARGS__)

//  Global severity macros (without this pointer)
#define ASIO_GLOBAL_ERROR(fmt, ...) ASIO_DBG ("ERROR", fmt, ##__VA_ARGS__)
#define ASIO_GLOBAL_WARN(fmt, ...) ASIO_DBG ("WARN", fmt, ##__VA_ARGS__)
#define ASIO_GLOBAL_INFO(fmt, ...) ASIO_DBG ("INFO", fmt, ##__VA_ARGS__)
#define ASIO_GLOBAL_DEBUG(fmt, ...) ASIO_DBG ("DEBUG", fmt, ##__VA_ARGS__)

#endif
