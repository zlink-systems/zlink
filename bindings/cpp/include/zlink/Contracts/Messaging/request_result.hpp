/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

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

} // namespace zlink
