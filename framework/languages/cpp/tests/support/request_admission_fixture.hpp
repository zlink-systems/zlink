/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include "runtime/messaging/envelope_codec.hpp"

namespace zlink::framework::test
{
template <typename T> request_task_t<T> accepted_request_task (task_t<T> reply)
{
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    admitted->complete (result_t<void>::success ());
    return request_task_t<T> (std::move (reply), std::move (admitted));
}

inline void
receive_request_reply (request_task_t<result_t<runtime::messaging::message_parts_t>> selected,
                       detail::request_receiver_t<runtime::messaging::message_parts_t> receiver)
{
    using parts_t = runtime::messaging::message_parts_t;
    auto admission = selected.admitted ();
    detail::observe_task_terminal (
      admission, [source = receiver.admitted] (const auto &result) { source->complete (result); });
    auto physical = detail::request_task_access_t::physical_reply (selected);
    detail::observe_task_terminal (physical, [complete = std::move (receiver.complete)] (
                                               const result_t<result_t<parts_t>> &result) {
        complete (result ? result.value ()
                         : detail::result_access_t::failure<parts_t> (result.exception ()));
    });
}

template <typename TRequest> auto request_receiver (TRequest request)
{
    return [request = std::move (request)] (
             const zlink::routing_id_t &target, runtime::messaging::message_parts_t parts,
             std::chrono::milliseconds timeout,
             detail::request_receiver_t<runtime::messaging::message_parts_t> receiver) mutable {
        receive_request_reply (request (target, std::move (parts), timeout), std::move (receiver));
    };
}

template <typename TRequest> auto channel_request_receiver (TRequest request)
{
    return [request = std::move (request)] (
             runtime::messaging::message_parts_t parts, std::chrono::milliseconds timeout,
             detail::request_receiver_t<runtime::messaging::message_parts_t> receiver) mutable {
        receive_request_reply (request (std::move (parts), timeout), std::move (receiver));
    };
}
}
