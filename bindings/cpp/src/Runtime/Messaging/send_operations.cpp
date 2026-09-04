/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include "completion_owner.hpp"
#include "operation_state.hpp"
#include "operation_submit.hpp"

#include <cerrno>

namespace zlink
{
namespace
{

struct send_completion_bundle_t
{
    explicit send_completion_bundle_t (
      std::unique_ptr<detail::operation_state_t> operation_) :
        result (), entry (&result, std::move (operation_))
    {
    }

    detail::async_operation_state_t<void> result;
    detail::completion_entry_t entry;
};

async_result_t<void> submit_send_awaitable (
  std::unique_ptr<detail::operation_state_t> state_)
{
    if (!state_ || (state_->kind != detail::operation_kind_t::raw_send
                    && state_->kind != detail::operation_kind_t::raw_routed_send)) {
        if (state_) {
            detail::restore_async_send_sources (*state_);
            detail::release_state (std::move (state_));
        }
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    }
    const auto runtime = detail::share_runtime_state (state_->raw);
    if (!runtime) {
        detail::restore_async_send_sources (*state_);
        detail::release_state (std::move (state_));
        throw submit_error_t (submit_result_t::invalid_state, ESHUTDOWN);
    }

    std::shared_ptr<send_completion_bundle_t> bundle;
    std::shared_ptr<detail::completion_entry_t> entry;
    try {
        bundle = std::make_shared<send_completion_bundle_t> (
          std::move (state_));
        entry = std::shared_ptr<detail::completion_entry_t> (
          bundle, &bundle->entry);
    }
    catch (...) {
        if (state_) {
            detail::restore_async_send_sources (*state_);
            detail::release_state (std::move (state_));
        }
        throw;
    }

    try {
        // Most DONTWAIT sends are admitted immediately. Submit first so that
        // path takes neither the completion-owner lock nor a waiter-map node.
        // A concurrent drain retains an early WRITABLE record by this entry's
        // unique context until registration replays it.
        if (!entry->start_send (true)) {
            runtime->completion->register_send_entry (entry);
            entry->detach_send_sources ();
        }
    }
    catch (...) {
        runtime->completion->unregister_entry (entry.get ());
        throw;
    }
    detail::async_result_state_t<void> *const result = &bundle->result;
    return detail::async_result_access_t::make<void> (
      std::shared_ptr<detail::async_result_state_t<void>> (
        std::move (bundle), result));
}

} // namespace

send_submit_operation_t::~send_submit_operation_t () = default;
send_submit_operation_t::send_submit_operation_t (send_submit_operation_t &&) noexcept = default;
send_submit_operation_t &
send_submit_operation_t::operator= (send_submit_operation_t &&) noexcept = default;

send_submit_operation_t &&send_submit_operation_t::message (message_t &part_) &&
{
    detail::append_send_part (state (), part_);
    return std::move (*this);
}

send_submit_operation_t &&send_submit_operation_t::message (message_t &&part_) &&
{
    detail::append_send_part (state (), std::move (part_));
    return std::move (*this);
}

send_operation_t::~send_operation_t () = default;
send_operation_t::send_operation_t (send_operation_t &&) noexcept = default;
send_operation_t &send_operation_t::operator= (send_operation_t &&) noexcept = default;

send_submit_operation_t send_operation_t::message (message_t &part_) &&
{
    state ().message.single_part_source = &part_;
    return send_submit_operation_t (release_state_ptr ());
}

send_submit_operation_t send_operation_t::message (message_t &&part_) &&
{
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    return send_submit_operation_t (release_state_ptr ());
}

void send_submit_operation_t::submit () &&
{
    auto &operation = state ();
    if (!detail::has_send_parts (operation))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    operation.flags = send_flags_t::none;
    if (!detail::submit_raw_send_state (operation))
        throw submit_error_t (submit_result_t::backpressured, EAGAIN);
}

async_result_t<void> send_submit_operation_t::async () &&
{
    auto &operation = state ();
    if (!detail::has_send_parts (operation))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    operation.flags = send_flags_t::dontwait;
    return submit_send_awaitable (release_state_ptr ());
}

publish_submit_operation_t::~publish_submit_operation_t () = default;
publish_submit_operation_t::publish_submit_operation_t (
  publish_submit_operation_t &&) noexcept = default;
publish_submit_operation_t &publish_submit_operation_t::operator= (
  publish_submit_operation_t &&) noexcept = default;

publish_submit_operation_t &&publish_submit_operation_t::message (message_t &part_) &&
{
    detail::append_send_part (state (), part_);
    return std::move (*this);
}

publish_submit_operation_t &&publish_submit_operation_t::message (message_t &&part_) &&
{
    detail::append_send_part (state (), std::move (part_));
    return std::move (*this);
}

publish_submit_operation_t &&publish_submit_operation_t::flags (int flags_) &&
{
    state ().flags = send_flags_t (flags_);
    return std::move (*this);
}

bool publish_submit_operation_t::submit () &&
{
    auto &operation = state ();
    if (!detail::has_send_parts (operation)
        || operation.kind != detail::operation_kind_t::raw_publish)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    return detail::submit_raw_send_state (operation);
}

publish_operation_t::~publish_operation_t () = default;
publish_operation_t::publish_operation_t (publish_operation_t &&) noexcept = default;
publish_operation_t &publish_operation_t::operator= (publish_operation_t &&) noexcept = default;

publish_submit_operation_t publish_operation_t::message (message_t &part_) &&
{
    state ().message.single_part_source = &part_;
    return publish_submit_operation_t (release_state_ptr ());
}

publish_submit_operation_t publish_operation_t::message (message_t &&part_) &&
{
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    return publish_submit_operation_t (release_state_ptr ());
}

} // namespace zlink
