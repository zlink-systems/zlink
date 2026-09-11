/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

namespace
{

using namespace zlink::framework;
using namespace std::chrono_literals;

static_assert (std::is_same_v<decltype (std::declval<request_call_t<int> &> ().submit ()), int>);
static_assert (std::is_same_v<decltype (std::declval<channel_request_call_t &> ().submit<int> ()), int>);
static_assert (std::is_same_v<decltype (std::declval<send_call_t &> ().submit ()), void>);
static_assert (std::is_same_v<decltype (std::declval<bound_session_send_call_t &> ().submit ()), void>);
static_assert (std::is_same_v<decltype (std::declval<stream_send_call_t &> ().submit ()), void>);
static_assert (std::is_same_v<decltype (std::declval<stream_write_call_t &> ().submit ()), void>);

template <typename Work> void expect_invalid_operation (Work &&work)
{
    try {
        work ();
        ADD_FAILURE () << "blocking submit must reject the runtime context";
    }
    catch (const framework_exception_t &error) {
        EXPECT_EQ (framework_error_kind_t::invalid_operation, error.kind ());
    }
}

// Keep the same calls across rejection and application-thread submission:
// rejection must precede preflight, transport work and single-use claims.
struct calls_t
{
    int preflights = 0;
    int requests = 0;
    int channel_requests = 0;
    int sends = 0;
    int bound_sends = 0;
    serializer_registry_t serializers;
    request_call_t<int> request{
      "request",
      [this] (const auto &, auto, const auto &) {
          ++requests;
          return task_t<int> (result_t<int>::success (42));
      },
      [this] (bool) {
          ++preflights;
          return result_t<void>::success ();
      }};
    channel_request_call_t channel_request{
      "channel-request", &serializers,
      [this] (const auto &, auto, const auto &) {
          ++channel_requests;
          return task_t<zlink::message_t> (
            result_t<zlink::message_t>::success (zlink::message_t::from (std::string ("42"))));
      }};
    send_call_t send{"send", [this] (const auto &, const auto &) {
                        ++sends;
                        return result_t<void>::success ();
                    }};
    bound_session_send_call_t bound_send{
      send_call_t{"bound-send", [this] (const auto &, const auto &) {
                      ++bound_sends;
                      return result_t<void>::success ();
                  }}};
    stream_send_call_t stream_send{result_t<void>::success ()};
    stream_write_call_t stream_write{result_t<void>::success ()};

    void expect_rejected ()
    {
        expect_invalid_operation ([&] { (void) request.submit (); });
        expect_invalid_operation ([&] { (void) channel_request.submit<int> (); });
        expect_invalid_operation ([&] { send.submit (); });
        expect_invalid_operation ([&] { bound_send.submit (); });
        expect_invalid_operation ([&] { stream_send.submit (); });
        expect_invalid_operation ([&] { stream_write.submit (); });
        EXPECT_EQ (0, preflights);
        EXPECT_EQ (0, requests);
        EXPECT_EQ (0, channel_requests);
        EXPECT_EQ (0, sends);
        EXPECT_EQ (0, bound_sends);
    }

    void expect_application_submission ()
    {
        EXPECT_EQ (42, request.submit ());
        EXPECT_EQ (42, channel_request.submit<int> ());
        EXPECT_NO_THROW (send.submit ());
        EXPECT_NO_THROW (bound_send.submit ());
        EXPECT_NO_THROW (stream_send.submit ());
        EXPECT_NO_THROW (stream_write.submit ());
        EXPECT_EQ (1, preflights);
        EXPECT_EQ (1, requests);
        EXPECT_EQ (1, channel_requests);
        EXPECT_EQ (1, sends);
        EXPECT_EQ (1, bound_sends);
    }
};

TEST (ZLinkFrameworkSyncSubmit, ApplicationThreadReturnsAllDocumentedResults)
{
    calls_t calls;
    calls.expect_application_submission ();
}

TEST (ZLinkFrameworkSyncSubmit, OneWayTerminalsShareTheSingleSubmissionClaim)
{
    // Submit/completion §2 leaves reuse to each operation's interface.
    // §5's one-way admission claim applies to these four call families.
    calls_t calls;
    calls.expect_application_submission ();
    expect_invalid_operation ([&] { calls.send.submit (); });
    expect_invalid_operation ([&] { calls.bound_send.submit (); });
    expect_invalid_operation ([&] { calls.stream_send.submit (); });
    expect_invalid_operation ([&] { calls.stream_write.submit (); });
    expect_invalid_operation ([&] { calls.send.async ().result ().value (); });
    expect_invalid_operation ([&] { calls.bound_send.async ().result ().value (); });
    expect_invalid_operation ([&] { calls.stream_send.async ().result ().value (); });
    expect_invalid_operation ([&] { calls.stream_write.async ().result ().value (); });
    EXPECT_EQ (1, calls.sends);
    EXPECT_EQ (1, calls.bound_sends);
}

struct channel_handler_t
{
    calls_t calls;
    detail::task_completion_source_t<void> resume;
    std::promise<void> entered;

    int handle (const int &value)
    {
        // A normal Channel handler has none of the Spot/Actor/lane markers.
        EXPECT_EQ (nullptr, detail::capture_current_serial_turn ());
        EXPECT_EQ (nullptr, detail::current_callback_context);
        EXPECT_EQ (nullptr, runtime::state_lane_t::current ());
        EXPECT_TRUE (runtime::current_actor_execution.actor_key.empty ());
        EXPECT_TRUE (runtime::current_actor_execution.spot_id.empty ());
        calls.expect_rejected ();
        return value;
    }

    task_t<void> handle_async (const int &value)
    {
        handle (value);
        entered.set_value ();
        co_await resume.task ();
        handle (value);
        co_return;
    }

    int handle_with_async_terminals (const int &value)
    {
        calls.expect_rejected ();
        EXPECT_EQ (42, calls.request.async ().result ().value ());
        EXPECT_EQ (42, calls.channel_request.async<int> ().result ().value ());
        EXPECT_TRUE (calls.send.async ().result ());
        EXPECT_TRUE (calls.bound_send.async ().result ());
        EXPECT_TRUE (calls.stream_send.async ().result ());
        EXPECT_TRUE (calls.stream_write.async ().result ());
        return value;
    }

    int complete_application_callbacks (const int &value)
    {
        resume.complete (result_t<void>::success ());
        return handle (value);
    }
};

TEST (ZLinkFrameworkSyncSubmit, ChannelHandlerRejectsBeforeMessageSubmission)
{
    service_collection_t services;
    services.add_singleton<channel_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    handler_registry_t handlers;
    handlers.on_request<channel_handler_t, int, int> (
      "sync-submit", "", &channel_handler_t::handle, {.packet_name = "request"});
    const auto result = handlers.invoke (
      "sync-submit", "request", provider, serializers,
      zlink::message_t::from (std::string ("42")));
    ASSERT_TRUE (result);
    EXPECT_EQ ("42", result.value ().to_string ());
    // Rejected calls remain unclaimed and submit exactly once outside the turn.
    provider.get_required<channel_handler_t> ().calls.expect_application_submission ();
}

TEST (ZLinkFrameworkSyncSubmit, AsyncChannelHandlerRejectsBeforeAndAfterSuspension)
{
    struct executor_scope_t
    {
        executor_scope_t () { runtime::configure_handler_coroutine_executor (1); }
        ~executor_scope_t () { runtime::shutdown_handler_coroutine_executor (); }
    } executor_scope;
    service_collection_t services;
    services.add_singleton<channel_handler_t> ();
    auto provider = services.build_provider ();
    auto &handler = provider.get_required<channel_handler_t> ();
    serializer_registry_t serializers;
    handler_registry_t handlers;
    handlers.on_send<channel_handler_t, int> (
      "sync-submit", "", &channel_handler_t::handle_async, {.packet_name = "request"});
    detail::channel_runtime_t channel (nullptr);
    auto result = channel.dispatch_send_async (
      "sync-submit", "", "request", provider, serializers, handlers,
      zlink::message_t::from (std::string ("42")), {});
    handler.entered.get_future ().wait ();
    // The single worker reaches this queued barrier only after the handler
    // suspends. Completion then resumes it on this application thread.
    std::promise<const void *> suspended;
    runtime::handler_coroutine_executor ().post_native_continuation ([&] {
        suspended.set_value (detail::application_job_context_t::current ());
    });
    EXPECT_EQ (nullptr, suspended.get_future ().get ());
    EXPECT_EQ (nullptr, detail::application_job_context_t::current ());
    handler.resume.complete (result_t<void>::success ());
    ASSERT_TRUE (result.result ());
    EXPECT_EQ (nullptr, detail::application_job_context_t::current ());
    handler.calls.expect_application_submission ();
}

TEST (ZLinkFrameworkSyncSubmit, ChannelHandlerKeepsAllAsyncTerminalsAvailable)
{
    service_collection_t services;
    services.add_singleton<channel_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    handler_registry_t handlers;
    handlers.on_request<channel_handler_t, int, int> (
      "sync-submit", "", &channel_handler_t::handle_with_async_terminals,
      {.packet_name = "request"});
    const auto result = handlers.invoke (
      "sync-submit", "request", provider, serializers,
      zlink::message_t::from (std::string ("42")));
    ASSERT_TRUE (result);
    EXPECT_EQ ("42", result.value ().to_string ());
    const auto &calls = provider.get_required<channel_handler_t> ().calls;
    EXPECT_EQ (1, calls.preflights);
    EXPECT_EQ (1, calls.requests);
    EXPECT_EQ (1, calls.channel_requests);
    EXPECT_EQ (1, calls.sends);
    EXPECT_EQ (1, calls.bound_sends);
}

TEST (ZLinkFrameworkSyncSubmit, ApplicationCallbacksCompletedInlineKeepTheHandlerContext)
{
    service_collection_t services;
    services.add_singleton<channel_handler_t> ();
    auto provider = services.build_provider ();
    auto &handler = provider.get_required<channel_handler_t> ();
    auto pending = handler.resume.task ();
    int completions = 0;
    int terminals = 0;
    ASSERT_EQ (nullptr, detail::application_job_context_t::current ());
    detail::observe_task_completion (pending, [&] (const result_t<void> &result) {
        EXPECT_TRUE (result);
        handler.calls.expect_rejected ();
        ++completions;
    });
    detail::observe_task_terminal (pending, [&] (const result_t<void> &result) {
        EXPECT_TRUE (result);
        handler.calls.expect_rejected ();
        ++terminals;
    });
    serializer_registry_t serializers;
    handler_registry_t handlers;
    handlers.on_request<channel_handler_t, int, int> (
      "sync-submit", "", &channel_handler_t::complete_application_callbacks,
      {.packet_name = "request"});
    ASSERT_TRUE (handlers.invoke (
      "sync-submit", "request", provider, serializers,
      zlink::message_t::from (std::string ("42"))));
    EXPECT_EQ (1, completions);
    EXPECT_EQ (1, terminals);
    EXPECT_EQ (nullptr, detail::application_job_context_t::current ());
    handler.calls.expect_application_submission ();
}

TEST (ZLinkFrameworkSyncSubmit, SerialHandlerTurnRejectsBeforeSideEffects)
{
    calls_t calls;
    runtime::offload_executor_t executor (1);
    runtime::serial_execution_queue_t queue (executor);
    queue.run ("sync-submit-handler", [&] {
        ASSERT_NE (nullptr, detail::capture_current_serial_turn ());
        calls.expect_rejected ();
    });
    calls.expect_application_submission ();
}

TEST (ZLinkFrameworkSyncSubmit, SpotCallbackRejectsBeforeSideEffects)
{
    calls_t calls;
    auto spot = std::make_shared<detail::spot_context_state_t> ();
    ASSERT_TRUE (spot->run_serial_sync ("sync-submit-spot", [&] {
        ASSERT_TRUE (spot->is_current_callback_thread ());
        // Exercise the Spot marker independently of the serial-turn marker.
        ASSERT_EQ (nullptr, detail::capture_current_serial_turn ());
        calls.expect_rejected ();
    }));
    calls.expect_application_submission ();
}

TEST (ZLinkFrameworkSyncSubmit, StateLaneRejectsBeforeSideEffects)
{
    calls_t calls;
    runtime::offload_executor_t executor (1);
    runtime::state_lane_t lane (executor);
    lane.run ([&] {
        ASSERT_EQ (&lane, runtime::state_lane_t::current ());
        calls.expect_rejected ();
    }).get ();
    calls.expect_application_submission ();
}

TEST (ZLinkFrameworkSyncSubmit, ActorContextRejectsBeforeSideEffects)
{
    calls_t calls;
    {
        runtime::actor_execution_scope_t scope ("actor", "spot");
        calls.expect_rejected ();
    }
    calls.expect_application_submission ();
}

TEST (ZLinkFrameworkSyncSubmit, RequestWaitsForApplicationReply)
{
    detail::task_completion_source_t<int> reply;
    std::promise<void> submitted;
    request_call_t<int> call{
      "request", [&] (const auto &packet, auto timeout, const auto &metadata) {
          EXPECT_EQ ("request", packet);
          EXPECT_EQ (250ms, timeout);
          EXPECT_EQ ("sync", metadata.at ("trace"));
          submitted.set_value ();
          return reply.task ();
      }};
    call.timeout (250ms).metadata ("trace", "sync");
    auto result = std::async (std::launch::async, [&] { return call.submit (); });
    submitted.get_future ().wait ();
    EXPECT_EQ (std::future_status::timeout, result.wait_for (0ms));
    reply.complete (result_t<int>::success (73));
    EXPECT_EQ (73, result.get ());
}

TEST (ZLinkFrameworkSyncSubmit, ChannelRequestWaitsForTypedApplicationReply)
{
    serializer_registry_t serializers;
    detail::task_completion_source_t<zlink::message_t> reply;
    std::promise<void> submitted;
    channel_request_call_t call{
      "channel-request", &serializers,
      [&] (const auto &, auto, const auto &) {
          submitted.set_value ();
          return reply.task ();
      }};
    auto result = std::async (std::launch::async, [&] { return call.submit<int> (); });
    submitted.get_future ().wait ();
    EXPECT_EQ (std::future_status::timeout, result.wait_for (0ms));
    reply.complete (result_t<zlink::message_t>::success (
      zlink::message_t::from (std::string ("73"))));
    EXPECT_EQ (73, result.get ());
}

TEST (ZLinkFrameworkSyncSubmit, StreamSendWaitsForAdmission)
{
    detail::stream_runtime_t runtime (std::make_shared<detail::stream_runtime_state_t> ());
    stream_t stream;
    detail::task_completion_source_t<void> admitted;
    std::promise<void> submitted;
    runtime.attach_transport_writer (
      stream, [&] (const auto &header, const auto &payload, auto timeout) {
          EXPECT_EQ ("sync-packet", header.packet_name ());
          EXPECT_EQ ("payload", payload.to_string ());
          EXPECT_EQ (250ms, timeout);
          submitted.set_value ();
          return admitted.task ();
      });
    auto call = stream.write_packet (zlink::message_t::from (std::string ("payload")));
    call.packet_name ("sync-packet").timeout (250ms);
    auto result = std::async (std::launch::async, [&] { call.submit (); });
    submitted.get_future ().wait ();
    EXPECT_EQ (std::future_status::timeout, result.wait_for (0ms));
    admitted.complete (result_t<void>::success ());
    EXPECT_NO_THROW (result.get ());
}

TEST (ZLinkFrameworkSyncSubmit, StreamSendPropagatesDeferredAdmissionFailure)
{
    detail::stream_runtime_t runtime (std::make_shared<detail::stream_runtime_state_t> ());
    stream_t stream;
    detail::task_completion_source_t<void> admitted;
    std::promise<void> submitted;
    runtime.attach_transport_writer (stream, [&] (const auto &, const auto &, auto) {
        submitted.set_value ();
        return admitted.task ();
    });
    auto call = stream.write_packet (zlink::message_t::from (std::string ("payload")));
    auto result = std::async (std::launch::async, [&] { call.submit (); });
    submitted.get_future ().wait ();
    admitted.complete (result_t<void>::failure (
      framework_error_kind_t::deadline_exceeded, "admission deadline"));
    try {
        result.get ();
        FAIL () << "submit must observe the deferred admission failure";
    }
    catch (const framework_exception_t &error) {
        EXPECT_EQ (framework_error_kind_t::deadline_exceeded, error.kind ());
        EXPECT_STREQ ("admission deadline", error.what ());
    }
}

class reply_session_t final : public packet_stream_session_t
{
  public:
    std::optional<stream_write_call_t> reply;

    task_t<void> on_connected (stream_t &) override { co_return; }
    task_t<void> on_disconnected (stream_t &) override { co_return; }
    task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }

    task_t<void> on_packet (stream_t &stream,
                            const session_message_context_t &,
                            const zlink::message_t &payload) override
    {
        reply.emplace (stream.reply_packet (payload));
        expect_invalid_operation ([&] { reply->submit (); });
        co_return;
    }
};

TEST (ZLinkFrameworkSyncSubmit, SessionHandlerRejectionPreservesReplyAdmission)
{
    struct dispatch_executor_scope_t
    {
        dispatch_executor_scope_t () { detail::configure_stream_dispatch_executor (); }
        ~dispatch_executor_scope_t () { detail::shutdown_stream_dispatch_executor (); }
    } dispatch_executor_scope;
    detail::stream_runtime_t runtime (std::make_shared<detail::stream_runtime_state_t> ());
    stream_t stream;
    reply_session_t session;
    detail::task_completion_source_t<void> admitted;
    std::promise<void> submitted;
    std::atomic_int writes{0};
    runtime.attach_transport_writer (
      stream, [&] (const auto &header, const auto &payload, auto) {
          ++writes;
          EXPECT_EQ (detail::stream_message_kind_t::response, header.kind ());
          EXPECT_EQ (12, header.request_seq ());
          EXPECT_EQ ("reply", payload.to_string ());
          submitted.set_value ();
          return admitted.task ();
      });
    detail::stream_header_t header (
      detail::stream_message_kind_t::request, stream_codec_t::raw,
      detail::stream_header_flags_t::has_request_seq, 12, "request", {});
    std::promise<result_t<void>> handled;
    ASSERT_TRUE (runtime.dispatch_packet_async (
      session, stream, header, zlink::message_t::from (std::string ("reply")),
      [&] (const result_t<void> &result) { handled.set_value (result); }));
    ASSERT_TRUE (handled.get_future ().get ());
    ASSERT_TRUE (session.reply);
    EXPECT_EQ (0, writes.load ());

    auto result = std::async (std::launch::async, [&] { session.reply->submit (); });
    submitted.get_future ().wait ();
    EXPECT_EQ (std::future_status::timeout, result.wait_for (0ms));
    admitted.complete (result_t<void>::success ());
    EXPECT_NO_THROW (result.get ());
    EXPECT_EQ (1, writes.load ());
    runtime.drain_async_dispatch (stream);
}

TEST (ZLinkFrameworkSyncSubmit, SubmissionFailuresKeepTheirFrameworkErrorKind)
{
    request_call_t<int> request{result_t<int>::failure (
      framework_error_kind_t::deadline_exceeded, "request deadline")};
    send_call_t send{result_t<void>::failure (
      framework_error_kind_t::unavailable, "send unavailable")};
    try {
        (void) request.submit ();
        FAIL () << "request failure must be thrown";
    }
    catch (const framework_exception_t &error) {
        EXPECT_EQ (framework_error_kind_t::deadline_exceeded, error.kind ());
        EXPECT_STREQ ("request deadline", error.what ());
    }
    try {
        send.submit ();
        FAIL () << "admission failure must be thrown";
    }
    catch (const framework_exception_t &error) {
        EXPECT_EQ (framework_error_kind_t::unavailable, error.kind ());
        EXPECT_STREQ ("send unavailable", error.what ());
    }
    expect_invalid_operation ([&] { send.submit (); });
}

} // namespace
