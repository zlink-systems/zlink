/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"

namespace zlink
{

/// @brief The outcome of registering or running a callback handler.
enum class handler_result_t : int
{
    ok = 0,
    invalid_argument = 301,
    busy = 302,
    not_supported = 303,
    deadlock = 304,
    invalid_handle = 305,
    internal_error = 306
};

/// @brief The outcome of closing a socket or resource.
enum class close_result_t : int
{
    ok = 0,
    busy = 401,
    shutdown = 402,
    invalid_handle = 403,
    internal_error = 404
};

/// @brief The outcome of binding a socket to an endpoint.
enum class bind_result_t : int
{
    ok = 0,
    invalid_argument = 501,
    addr_in_use = 502,
    not_supported = 503,
    invalid_handle = 504,
    internal_error = 505
};

/// @brief The outcome of connecting a socket to an endpoint.
enum class connect_result_t : int
{
    ok = 0,
    invalid_argument = 601,
    not_supported = 602,
    invalid_handle = 603,
    internal_error = 604,
    not_found = 605,
    conflict = 606,
    busy = 607
};

/// @brief The outcome of reading or applying a configuration option.
enum class config_result_t : int
{
    ok = 0,
    invalid_handle = 701,
    invalid_argument = 702,
    not_supported = 703,
    internal_error = 704,
    invalid_state = 705,
    not_found = 706
};

} // namespace zlink
