/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <cstdint>

namespace zlink
{

/// @brief Readiness conditions a poll source can be watched for or report.
enum class poll_event_flag_t : short
{
    none = 0,
    pollin = 1,         ///< Readable: a receive will not block.
    pollout = 2,        ///< Writable: a send will not block.
    pollerr = 4,        ///< An error condition occurred on the source.
    pollcompletion = 32 ///< An asynchronous operation completed.
};

inline poll_event_flag_t operator| (poll_event_flag_t a_, poll_event_flag_t b_)
{
    return static_cast<poll_event_flag_t> (static_cast<short> (a_) | static_cast<short> (b_));
}

/// @brief Identifies what kind of source a poll event came from.
enum class poll_source_kind_t : int
{
    socket = 1, ///< A socket.
    fd = 2,     ///< A raw file descriptor.
    timer = 3   ///< A timer.
};

/// @brief One ready source reported by a poller wait.
struct poll_event_t
{
    poll_source_kind_t source_kind = poll_source_kind_t::socket;
    std::uintptr_t slot = 0;
    poll_event_flag_t revents = poll_event_flag_t::none;
    int fd = 0;
};

} // namespace zlink
