/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include "completion_owner.hpp"
#include "operation_state.hpp"
#include "operation_submit.hpp"

#include <cerrno>

namespace zlink
{
request_submit_operation_t::~request_submit_operation_t () = default;
request_submit_operation_t::request_submit_operation_t (request_submit_operation_t &&) noexcept =
  default;
request_submit_operation_t &
request_submit_operation_t::operator= (request_submit_operation_t &&) noexcept = default;

request_submit_operation_t &&request_submit_operation_t::message (message_t &part_) &&
{
    detail::append_send_part (state (), part_);
    return std::move (*this);
}

request_submit_operation_t &&request_submit_operation_t::message (message_t &&part_) &&
{
    detail::append_send_part (state (), std::move (part_));
    return std::move (*this);
}

request_submit_operation_t &&
request_submit_operation_t::timeout (std::chrono::milliseconds timeout_) &&
{
    if (timeout_ < std::chrono::milliseconds::zero ())
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    state ().timeout = timeout_;
    return std::move (*this);
}

request_operation_t::~request_operation_t () = default;
request_operation_t::request_operation_t (request_operation_t &&) noexcept = default;
request_operation_t &request_operation_t::operator= (request_operation_t &&) noexcept = default;

request_submit_operation_t request_operation_t::message (message_t &part_) &&
{
    state ().message.single_part_source = &part_;
    return request_submit_operation_t (release_state_ptr ());
}

request_submit_operation_t request_operation_t::message (message_t &&part_) &&
{
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    return request_submit_operation_t (release_state_ptr ());
}

std::vector<message_t> request_submit_operation_t::submit () &&
{
    auto &operation = state ();
    if (!detail::has_send_parts (operation))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    const auto runtime = detail::share_runtime_state (operation.raw);
    if (!runtime)
        throw submit_error_t (submit_result_t::invalid_state, ESHUTDOWN);

    std::shared_ptr<detail::async_operation_state_t<std::vector<message_t>>> no_async;
    auto entry = std::make_shared<detail::completion_entry_t> (no_async);
    runtime->completion->register_entry (entry);
    try {
        operation.flags = send_flags_t::none;
        zlink_completion_id_t completion_id = 0;
        (void) detail::submit_raw_request_state (
          operation, entry.get (), &completion_id);
        entry->publish (completion_id);
    }
    catch (...) {
        entry->fail_submit ();
        runtime->completion->unregister_entry (entry.get ());
        throw;
    }
    return entry->wait_request ();
}

async_result_t<std::vector<message_t>> request_submit_operation_t::async () &&
{
    auto &operation = state ();
    if (!detail::has_send_parts (operation))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    const auto runtime = detail::share_runtime_state (operation.raw);
    if (!runtime)
        throw submit_error_t (submit_result_t::invalid_state, ESHUTDOWN);

    operation.flags = send_flags_t::dontwait;
    auto result = std::make_shared<
      detail::async_operation_state_t<std::vector<message_t>>> ();
    auto operation_state = release_state_ptr ();
    std::shared_ptr<detail::completion_entry_t> entry;
    try {
        entry = std::make_shared<detail::completion_entry_t> (
          result, std::move (operation_state));
    }
    catch (...) {
        if (operation_state) {
            detail::restore_async_send_sources (*operation_state);
            detail::release_state (std::move (operation_state));
        }
        throw;
    }
    runtime->completion->register_entry (entry);
    try {
        entry->start_request ();
    }
    catch (...) {
        entry->fail_submit ();
        runtime->completion->unregister_entry (entry.get ());
        throw;
    }
    return detail::async_result_access_t::make<std::vector<message_t>> (
      std::move (result));
}

} // namespace zlink
