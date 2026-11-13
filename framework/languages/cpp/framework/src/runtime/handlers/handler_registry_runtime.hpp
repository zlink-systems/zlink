/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <memory>
#include <zlink/framework/contracts/messaging/message_context.hpp>

namespace zlink::framework::detail
{

inline bool is_request_dispatch (handler_dispatch_kind_t kind) noexcept
{
    return kind == handler_dispatch_kind_t::node_direct_request
           || kind == handler_dispatch_kind_t::channel_request;
}

struct prepared_filter_terminal_t
{
    std::shared_ptr<void> target;
    task_t<zlink::message_t> (*invoke) (const std::shared_ptr<void> &);
    task_t<zlink::message_t> operator() () const { return invoke (target); }
};

std::shared_ptr<runtime::offload_executor_t> handler_invocation_executor ();

} // namespace zlink::framework::detail
