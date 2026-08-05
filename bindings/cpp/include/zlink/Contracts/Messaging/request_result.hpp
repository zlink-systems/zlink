/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "message.hpp"

#include <functional>
#include <vector>

namespace zlink
{

/// @brief The outcome of a request.
enum class request_result_t : int
{
    ok = 0,
    timed_out = 101,
    not_found = 102,
    terminated = 103,
    protocol_error = 104,
    internal_error = 105,
    rejected = 106,
    conflict = 107,
    busy = 108,
    not_connected = 109,
    invalid_argument = 110,
    invalid_state = 111,
    not_supported = 112
};

/// @brief Receives request completion and reply payloads.
/// @note On @c ok, the callback owns the messages in the vector and must call close() on each.
using request_callback_t = std::function<void (request_result_t, std::vector<message_t>)>;

} // namespace zlink
