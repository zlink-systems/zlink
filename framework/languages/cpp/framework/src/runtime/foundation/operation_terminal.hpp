/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

namespace zlink::framework::runtime::foundation
{

enum class operation_terminal_t
{
    completed,
    timed_out,
    cancelled,
    transport_failed,
    protocol_error,
    shutdown,
    route_unavailable
};

} // namespace zlink::framework::runtime::foundation
