/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/backend/raw_route_port.hpp"

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include <optional>
#include <exception>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace zlink::framework::detail::backend
{

struct binding_completion_observer_t
{
    struct promise_type
    {
        binding_completion_observer_t get_return_object () noexcept { return {}; }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_void () noexcept {}
        void unhandled_exception () noexcept { std::terminate (); }

        zlink::framework::detail::task_scheduler_t zlink_continuation_scheduler () const
        {
            // Binding completion already arrives on its owning completion
            // resource. Raw transport classification runs there and hands the
            // terminal directly to the reserved Framework dispatcher item.
            return {};
        }
    };
};

template <typename TSubmission>
std::optional<zlink::async_result_t<void>> take_submission_admission (TSubmission &submission)
{
    if (submission.result == ZLINK_SUBMIT_BACKPRESSURED)
        return std::move (submission.admitted);
    if (submission.result != ZLINK_SUBMIT_OK)
        throw std::logic_error ("binding async operation returned an invalid result snapshot");
    return std::nullopt;
}

struct request_submission_stages_t
{
    std::optional<zlink::async_result_t<void>> admission;
    zlink::async_result_t<std::vector<zlink::message_t>> reply;
    std::optional<std::chrono::steady_clock::time_point> caller_deadline;
};

template <typename TSubmit>
request_submission_stages_t submit_request_once (
  TSubmit &&submit,
  std::optional<std::chrono::steady_clock::time_point> caller_deadline = std::nullopt)
{
    auto submission = std::forward<TSubmit> (submit) ();
    auto admission = take_submission_admission (submission);
    // Core bounds the reply after immediate admission. Only its wait-token
    // result needs a Framework deadline covering admission and the later reply.
    if (!admission)
        caller_deadline.reset ();
    return {std::move (admission), std::move (submission.reply), caller_deadline};
}

// A successful binding receive owns native parts until close(). This guard
// pairs that terminal release with the receive even when ownership transfer or
// metadata validation throws.
template <typename TReceived> class binding_received_release_t final
{
  public:
    explicit binding_received_release_t (TReceived &received) noexcept : _received (&received) {}

    binding_received_release_t (const binding_received_release_t &) = delete;
    binding_received_release_t &operator= (const binding_received_release_t &) = delete;

    ~binding_received_release_t () noexcept
    {
        try {
            _received->close ();
        }
        catch (...) {
        }
    }

  private:
    TReceived *_received;
};

// The C++ binding receive API retains its native message storage. This is the
// single binding-facing ownership boundary: it copies each part once into the
// Framework-owned representation before the binding receive envelope closes.
inline raw_message_t copy_binding_parts (const std::vector<zlink::message_t> &parts)
{
    raw_message_t result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (part.to_bytes ());
    }
    return result;
}

inline binding_completion_observer_t observe_request_completion (
  request_submission_stages_t stages,
  std::shared_ptr<task_completion_source_t<raw_request_completion_t>> source)
{
    try {
        std::exception_ptr admission_error;
        if (stages.admission) {
            try {
                co_await std::move (*stages.admission);
            }
            catch (const std::exception &) {
                admission_error = std::current_exception ();
            }
        }
        auto reply = co_await std::move (stages.reply);
        if (admission_error)
            std::rethrow_exception (admission_error);
        source->complete (result_t<raw_request_completion_t>::success (
          raw_request_completion_t{zlink::request_result_t::ok, copy_binding_parts (reply)}));
    }
    catch (const zlink::request_error_t &error) {
        source->complete (result_t<raw_request_completion_t>::success (raw_request_completion_t{
          error.result (),
          {},
          raw_request_failure_t{raw_request_failure_phase_t::completion_terminal, std::nullopt,
                                error.internal_errno ()}}));
    }
    catch (const zlink::submit_error_t &error) {
        const auto result = runtime::messaging::map_submit_request_result (error.result (), true);
        source->complete (result_t<raw_request_completion_t>::success (raw_request_completion_t{
          result,
          {},
          raw_request_failure_t{raw_request_failure_phase_t::completion_terminal, error.result (),
                                error.internal_errno ()}}));
    }
    catch (const std::exception &error) {
        source->complete (result_t<raw_request_completion_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        source->complete (result_t<raw_request_completion_t>::failure (
          framework_error_kind_t::internal_failure, "raw route request completion failed"));
    }
}

inline std::vector<zlink::message_t>
copy_binding_messages (const std::vector<zlink::message_t> &parts)
{
    std::vector<zlink::message_t> result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (part);
    }
    return result;
}

inline std::vector<zlink::message_t> materialize_binding_parts (raw_message_t parts)
{
    std::vector<zlink::message_t> result;
    result.reserve (parts.size ());
    for (auto &part : parts) {
        auto storage = std::make_unique<raw_bytes_t> (std::move (part));
        auto message = zlink::advanced::external_message_t::from (
          std::span<std::uint8_t> (*storage),
          [] (void *, void *hint) { delete static_cast<raw_bytes_t *> (hint); }, storage.get ());
        if (!message.valid ())
            throw std::bad_alloc ();
        storage.release ();
        result.push_back (std::move (message));
    }
    return result;
}

} // namespace zlink::framework::detail::backend
