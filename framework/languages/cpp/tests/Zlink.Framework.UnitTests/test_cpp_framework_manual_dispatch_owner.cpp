/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/channel_packet_dispatcher.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "test_runtime_options_owner_pause.hpp"
#include "runtime/channels/channel_host_service_test_access.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "test_completion_poller_driver.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include <iostream>
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include <zlink/framework.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace
{
namespace fw = zlink::framework;
namespace messaging = fw::runtime::messaging;

struct payload_t
{
    int value = 0;
};

struct owner_handler_t
{
    std::thread::id entered_on;
    unsigned calls = 0;

    payload_t request (const payload_t &payload)
    {
        entered_on = std::this_thread::get_id ();
        ++calls;
        return payload_t{payload.value + 1};
    }

    void send (const payload_t &)
    {
        entered_on = std::this_thread::get_id ();
        ++calls;
    }
};

void check_synchronous_handler_owner (bool request)
{
    fw::zlink_builder_t builder;
    // This records configuration only. No runtime is started and no socket or Core context is made.
    builder.channel ("manual").enable_server ().bind ("inproc://manual-dispatch-owner-unused");
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    fw::detail::channel_packet_dispatcher_t dispatcher (runtime);
    fw::serializer_registry_t serializers;
    serializers.add<payload_t> (
      [] (const payload_t &payload) {
          return fw::encoded_payload_t::from_string (std::to_string (payload.value));
      },
      [] (const fw::encoded_payload_t &payload) {
          return payload_t{std::stoi (payload.to_string ())};
      },
      "application/json");
    runtime.bind_serializers (serializers);
    fw::service_collection_t services;
    services.add_singleton<owner_handler_t> ();
    auto provider = services.build_provider ();
    auto &handler = provider.get_required<owner_handler_t> ();
    fw::handler_registry_t handlers;
    handlers.on_request<owner_handler_t, payload_t, payload_t> (
      "manual", "", &owner_handler_t::request, {.packet_name = "request"});
    handlers.on_send<owner_handler_t, payload_t> ("manual", "", &owner_handler_t::send,
                                                  {.packet_name = "send"});
    messaging::envelope_header_t header;
    header.channel_name = "manual";
    header.kind = request ? messaging::message_kind_t::request : messaging::message_kind_t::command;
    header.message_name = request ? "request" : "send";
    header.correlation_id = "manual-owner";
    const auto parts =
      messaging::envelope_codec_t{}.encode_parts (header, payload_t{7}, serializers);

    fw::runtime::offload_executor_t application_worker (1, "manual-dispatch-owner");
    std::promise<fw::result_t<messaging::message_parts_t>> finished;
    auto result = finished.get_future ();
    std::thread::id worker_thread;
    application_worker.submit ([&] {
        worker_thread = std::this_thread::get_id ();
        try {
            finished.set_value (dispatcher.dispatch_server_message ("manual", parts, provider,
                                                                    serializers, handlers));
        }
        catch (...) {
            finished.set_exception (std::current_exception ());
        }
    });
    const auto reply = result.get ();
    application_worker.drain ();
    ASSERT_TRUE (reply);
    ASSERT_EQ (1u, handler.calls);
    EXPECT_EQ (worker_thread, handler.entered_on)
      << "the granted application worker reposted the record to another executor";
    if (request) {
        const auto body = messaging::envelope_codec_t{}.decode_body (reply.value ());
        ASSERT_TRUE (body);
        EXPECT_EQ ("8", body.value ().to_string ());
    } else {
        EXPECT_EQ (0u, reply.value ().size ());
    }
}

TEST (ManualDispatchOwner, SynchronousRequestPreservesApplicationWorker)
{
    check_synchronous_handler_owner (true);
}

TEST (ManualDispatchOwner, SynchronousSendPreservesApplicationWorker)
{
    check_synchronous_handler_owner (false);
}
} // namespace

TEST (ManualDispatchOwner, PendingTerminalRetainsDrainWithoutOccupyingWorker)
{
    using namespace std::chrono_literals;
    for (const bool fail : {false, true}) {
        SCOPED_TRACE (fail);
        fw::runtime::offload_executor_t worker (1, "manual-terminal-drain");
        auto terminal = std::make_shared<fw::task_completion_source_t<void>> ();
        struct release_t
        {
            std::shared_ptr<fw::task_completion_source_t<void>> terminal;
            ~release_t () { terminal->complete (fw::result_t<void>::success ()); }
        } release{terminal};
        auto entered = std::make_shared<std::promise<void>> ();
        auto started = entered->get_future ();
        auto terminal_observed = std::make_shared<std::atomic_bool> (false);
        worker.submit ([&worker, terminal, entered, terminal_observed] {
            auto retained = std::make_shared<fw::runtime::offload_executor_t::retained_work_t> (
              worker.retain_internal_work ());
            auto pending = terminal->task ();
            fw::detail::observe_task_terminal (
              pending, [retained = std::move (retained), terminal_observed] (const auto &) mutable {
                  terminal_observed->store (true, std::memory_order_release);
                  retained.reset ();
              });
            entered->set_value ();
        });
        ASSERT_EQ (std::future_status::ready, started.wait_for (2s));
        auto next = std::make_shared<std::promise<void>> ();
        auto progressed = next->get_future ();
        worker.submit ([next] { next->set_value (); });
        ASSERT_EQ (std::future_status::ready, progressed.wait_for (2s));
        EXPECT_FALSE (terminal->task ().await_ready ());
        EXPECT_FALSE (worker.drain_until (std::chrono::steady_clock::now ()))
          << "drain returned before an accepted handler's physical terminal";
        terminal->complete (
          fail ? fw::result_t<void>::failure (fw::framework_error_kind_t::internal_failure,
                                              "terminal test failure")
               : fw::result_t<void>::success ());
        EXPECT_TRUE (worker.drain_until (std::chrono::steady_clock::now () + 2s));
        EXPECT_TRUE (terminal_observed->load (std::memory_order_acquire));
        EXPECT_TRUE (worker.drained ());
    }
}

TEST (ManualDispatchOwner, RuntimeOptionReadDoesNotWaitForStateOwner)
{
    using namespace std::chrono_literals;
    fw::zlink_builder_t builder;
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    auto received = std::make_shared<std::promise<std::optional<int>>> ();
    auto result = received->get_future ();
    std::jthread reader;
    {
        fw::test::runtime_options_owner_pause_t pause (runtime, "manual", 37);
        ASSERT_TRUE (pause.wait_until_entered (2s));
        reader = std::jthread ([runtime, received] {
            try {
                received->set_value (runtime.server_peer_weight_override ("manual"));
            }
            catch (...) {
                received->set_exception (std::current_exception ());
            }
        });
        EXPECT_EQ (std::future_status::ready, result.wait_for (2s))
          << "receive option read waited for the occupied state owner";
        EXPECT_TRUE (pause.still_paused ());
    }
    reader.join ();
    EXPECT_EQ (std::optional<int> (37), result.get ());
}

namespace
{
struct fifo_handler_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<int> entries;
    std::vector<int> pipeline_entries;
    bool fault_matrix = false;
    std::shared_future<void> prefix_block;
    std::atomic_int scopes_created{0};
    std::atomic_int scopes_destroyed{0};
    fw::task_completion_source_t<void> release_filter;
    fw::task_completion_source_t<void> release_first;
    fw::task_t<payload_t> request (const payload_t &payload)
    {
        {
            std::lock_guard lock (mutex);
            entries.push_back (payload.value);
        }
        changed.notify_all ();
        if (fault_matrix && payload.value == 4)
            throw std::runtime_error ("native manual handler failure");
        if (payload.value == 1)
            co_await release_first.task ();
        co_return payload_t{payload.value + 100};
    }
};

struct scope_probe_t
{
    explicit scope_probe_t (fifo_handler_t &state) : state (state) { ++state.scopes_created; }
    ~scope_probe_t ()
    {
        {
            std::lock_guard lock (state.mutex);
            ++state.scopes_destroyed;
        }
        state.changed.notify_all ();
    }
    fifo_handler_t &state;
};

struct pending_filter_t
{
    pending_filter_t (fifo_handler_t &state, scope_probe_t &) : state (state) {}
    fw::task_t<void> invoke (const fw::handler_filter_context_t &context, fw::handler_next_t next)
    {
        const auto value = std::stoi (context.correlation_id.value ());
        {
            std::lock_guard lock (state.mutex);
            state.pipeline_entries.push_back (value);
        }
        state.changed.notify_all ();
        if (value == 1) {
            if (state.prefix_block.valid ())
                state.prefix_block.wait ();
            co_await state.release_filter.task ();
        }
        if (state.fault_matrix && value == 5)
            throw std::runtime_error ("native manual filter failure");
        co_await next ();
    }
    fifo_handler_t &state;
};

template <typename T> fw::task_t<T> native_result (zlink::async_result_t<T> pending)
{
    co_return co_await std::move (pending);
}
}

namespace
{
struct dispatch_observations_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<fw::message_dispatch_error_event_t> errors;
    std::vector<fw::message_flow_event_t> flows;
};

struct native_manual_fixture_t
{
    explicit native_manual_fixture_t (std::unique_ptr<fw::runtime::offload_executor_t> worker,
                                      bool with_filter,
                                      std::uint32_t capacity = static_cast<std::uint32_t> (
                                        fw::runtime::dispatch_limits::receive_batch_messages)) :
        builder (fw::test::runtime_failure_builder ()),
        runtime (fw::detail::channel_runtime_t::from (builder.message_bus ())),
        context (std::make_shared<zlink::context_t> ())
    {
        builder.channel ("manual").enable_server ().bind ("tcp://127.0.0.1:0");
        runtime.bind_core_context (context);
        serializers.add<payload_t> (
          [] (const payload_t &p) {
              return fw::encoded_payload_t::from_string (std::to_string (p.value));
          },
          [] (const fw::encoded_payload_t &p) { return payload_t{std::stoi (p.to_string ())}; },
          "application/json");
        runtime.bind_serializers (serializers);
        fw::dispatch_options_t options;
        options.message_flow (fw::message_flow_log_mode_t::normal);
        fw::detail::dispatch_options_access_t::set_observer_for_tests (
          options, [observed = diagnostics] (const fw::message_flow_event_t &event) {
              std::lock_guard lock (observed->mutex);
              observed->flows.push_back (event);
              observed->changed.notify_all ();
          });
        fw::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          options, [observed = diagnostics] (const fw::message_dispatch_error_event_t &event) {
              std::lock_guard lock (observed->mutex);
              observed->errors.push_back (event);
              observed->changed.notify_all ();
          });
        fw::detail::apply_dispatch_options (builder, options);
        services.add_singleton<fifo_handler_t> ();
        services.add_scoped<scope_probe_t, fifo_handler_t> ();
        services.add_transient<pending_filter_t, fifo_handler_t, scope_probe_t> ();
        provider = services.build_provider ();
        handler = &provider.get_required<fifo_handler_t> ();
        handlers.on_request<fifo_handler_t, payload_t, payload_t> (
          "manual", "", &fifo_handler_t::request, {.packet_name = "request"});
        if (with_filter)
            handlers.use_filter<pending_filter_t> ();
        jobs = std::make_shared<fw::runtime::application_job_queue_t> (
          fw::runtime::application_job_queue_configuration_t{
            fw::application_job_queue_profile_t::balanced, std::nullopt, 1, capacity});
        listeners = std::make_shared<fw::runtime::listener_status_registry_t> ();
        executor = worker.get ();
        held = dynamic_cast<fw::test::manual_internal_scheduler_t *> (executor);
        server = std::make_unique<fw::runtime::manual_channel_server_test_access_t> (
          builder.message_bus (), runtime.channel_snapshots ().front (), provider, serializers,
          handlers, stopping, context, jobs, listeners, std::move (worker));
        client = std::make_unique<zlink::dealer_socket_t> (*context);
        completions = std::make_unique<fw::test::completion_poller_driver_t> (*client);
        client->connect (listeners->find (fw::listener_kind_t::client_server, "manual")->endpoint);
    }

    ~native_manual_fixture_t ()
    {
        handler->release_filter.complete (fw::result_t<void>::success ());
        handler->release_first.complete (fw::result_t<void>::success ());
        if (held)
            held->run_all ();
        stopping.store (true, std::memory_order_release);
        server->signal_activity ();
        if (receiver.joinable ())
            receiver.join ();
        if (held)
            held->run_all ();
        if (stopped.valid ()) {
            try {
                stopped.get ();
            }
            catch (const std::exception &error) {
                ADD_FAILURE () << error.what ();
            }
            catch (...) {
                ADD_FAILURE () << "manual receive loop threw";
            }
        }
    }

    void request (int value, std::string packet = "request", std::optional<std::string> body = {})
    {
        constexpr auto timeout = std::chrono::seconds (2);
        messaging::envelope_header_t header;
        header.channel_name = "manual";
        header.kind = messaging::message_kind_t::request;
        header.message_name = std::move (packet);
        header.correlation_id = std::to_string (value);
        auto parts =
          body ? messaging::envelope_codec_t{}.encode_raw_body_parts (
                   header, zlink::message_t::from (*body))
               : messaging::envelope_codec_t{}.encode_parts (header, payload_t{value}, serializers);
        auto messages = std::move (parts).take_items ();
        auto submission =
          client->request ().message (messages[0]).message (messages[1]).timeout (timeout).async ();
        // Confirm local admission before submitting the next FIFO request.
        auto admitted = native_result (std::move (submission.admitted)).result_for (timeout);
        if (!admitted)
            throw std::runtime_error ("native request admission timed out");
        admitted->value ();
        replies.push_back (native_result (std::move (submission.reply)));
    }

    void dump_diagnostics () const
    {
        std::lock_guard lock (diagnostics->mutex);
        for (const auto &flow : diagnostics->flows)
            std::cout << "native-flow outcome=" << fw::detail::enum_name (flow.outcome)
                      << " flow=" << flow.flow_id.value_or ("")
                      << " corr=" << flow.correlation_id.value_or ("")
                      << " packet=" << flow.packet_name.value_or ("") << '\n';
        for (const auto &error : diagnostics->errors)
            std::cout << "native-error reason=" << fw::detail::enum_name (error.reason)
                      << " action=" << fw::detail::enum_name (error.action)
                      << " corr=" << error.correlation_id.value_or ("")
                      << " errorType=" << error.error_type.value_or ("")
                      << " errorMessage=" << error.error_message.value_or ("") << '\n';
    }

    void start ()
    {
        receiver = std::thread ([this] {
            try {
                server->run ();
                finished.set_value ();
            }
            catch (...) {
                finished.set_exception (std::current_exception ());
            }
        });
    }

    fw::completion_owner_t completion_owner;
    fw::zlink_builder_t builder;
    fw::detail::channel_runtime_t runtime;
    std::shared_ptr<zlink::context_t> context;
    fw::serializer_registry_t serializers;
    fw::service_collection_t services;
    fw::service_provider_t provider;
    fw::handler_registry_t handlers;
    fifo_handler_t *handler = nullptr;
    std::shared_ptr<fw::runtime::application_job_queue_t> jobs;
    std::shared_ptr<fw::runtime::listener_status_registry_t> listeners;
    fw::runtime::offload_executor_t *executor = nullptr;
    fw::test::manual_internal_scheduler_t *held = nullptr;
    std::shared_ptr<dispatch_observations_t> diagnostics =
      std::make_shared<dispatch_observations_t> ();
    std::atomic_bool stopping{false};
    std::unique_ptr<fw::runtime::manual_channel_server_test_access_t> server;
    std::unique_ptr<zlink::dealer_socket_t> client;
    std::unique_ptr<fw::test::completion_poller_driver_t> completions;
    std::vector<fw::task_t<std::vector<zlink::message_t>>> replies;
    std::promise<void> finished;
    std::future<void> stopped{finished.get_future ()};
    std::thread receiver;
};
} // namespace

void check_native_fifo (bool held_worker, bool async_filter, bool reject = false)
{
    using namespace std::chrono_literals;
    std::unique_ptr<fw::runtime::offload_executor_t> worker;
    if (held_worker)
        worker = std::make_unique<fw::test::manual_internal_scheduler_t> ();
    else
        worker = std::make_unique<fw::runtime::offload_executor_t> (4, "manual-fifo-workers");
    native_manual_fixture_t fixture (std::move (worker), async_filter);
    auto &handler = *fixture.handler;
    auto jobs = fixture.jobs;
    auto held = fixture.held;
    auto executor = fixture.executor;
    auto &server = *fixture.server;
    auto &replies = fixture.replies;
    if (reject)
        held->retire ();
    for (int value = 1; value <= (reject ? 1 : 3); ++value)
        fixture.request (value);
    fixture.start ();
    if (reject) {
        {
            std::unique_lock lock (fixture.diagnostics->mutex);
            ASSERT_TRUE (fixture.diagnostics->changed.wait_for (
              lock, 2s, [&] { return !fixture.diagnostics->errors.empty (); }));
            ASSERT_EQ (1u, fixture.diagnostics->errors.size ());
            EXPECT_EQ (fw::dispatch_error_reason_t::shutdown,
                       fixture.diagnostics->errors.front ().reason);
            EXPECT_EQ (fw::dispatch_error_action_t::drop,
                       fixture.diagnostics->errors.front ().action);
        }
        const auto watchdog = std::chrono::steady_clock::now () + 2s;
        while (jobs->snapshot ().queued_application_jobs != 0
               && std::chrono::steady_clock::now () < watchdog)
            std::this_thread::yield ();
        EXPECT_EQ (0u, jobs->snapshot ().queued_application_jobs);
        EXPECT_EQ (0u, server.pending_messages ());
        EXPECT_EQ (0u, held->pending_work ());
        EXPECT_TRUE (handler.entries.empty ());
        EXPECT_NO_THROW (fixture.runtime.runtime_failures ()->rethrow_if_failed ());
        return;
    }
    if (held) {
        const auto watchdog = std::chrono::steady_clock::now () + 2s;
        while (!(jobs->snapshot ().queued_application_jobs == 3
                 && (held->pending_work () == 3
                     || (server.pending_messages () == 3 && held->pending_work () != 0)))
               && std::chrono::steady_clock::now () < watchdog)
            std::this_thread::yield ();
        ASSERT_EQ (3u, jobs->snapshot ().queued_application_jobs) << "native DATA acceptance setup";
        // The baseline acknowledges three receives through the existing permit observation,
        // even though it bypasses the mailbox. Failure must be this work count, not a timeout.
        std::cout << "manual coalescing accepted=" << jobs->snapshot ().queued_application_jobs
                  << " mailbox=" << server.pending_messages ()
                  << " posted=" << held->pending_work () << '\n';
        ASSERT_EQ (1u, held->pending_work ()) << "per-record work replaced owner readiness";
        EXPECT_EQ (3u, server.pending_messages ());
        held->run_all ();
    }
    {
        std::unique_lock lock (handler.mutex);
        ASSERT_TRUE (handler.changed.wait_for (lock, 2s, [&] {
            return async_filter ? handler.pipeline_entries.size () == 3
                                    && handler.scopes_destroyed.load () == 2
                                : handler.entries.size () == 3;
        }));
        if (async_filter)
            EXPECT_EQ ((std::vector<int>{1, 2, 3}), handler.pipeline_entries);
        else
            EXPECT_EQ ((std::vector<int>{1, 2, 3}), handler.entries);
    }
    EXPECT_FALSE (executor->drain_until (std::chrono::steady_clock::now ()))
      << "an actual native invocation must retain physical-terminal drain ownership";
    if (async_filter) {
        EXPECT_EQ (1u, jobs->snapshot ().queued_application_jobs)
          << "the first async filter must retain its first-instruction permit";
        EXPECT_EQ (3, handler.scopes_created.load ());
        EXPECT_EQ (2, handler.scopes_destroyed.load ());
        handler.release_filter.complete (fw::result_t<void>::success ());
        std::unique_lock lock (handler.mutex);
        ASSERT_TRUE (
          handler.changed.wait_for (lock, 2s, [&] { return handler.entries.size () == 3; }));
        EXPECT_EQ ((std::vector<int>{2, 3, 1}), handler.entries);
    }
    EXPECT_FALSE (replies[0].await_ready ());
    EXPECT_EQ (0u, jobs->snapshot ().queued_application_jobs);
    handler.release_first.complete (fw::result_t<void>::success ());
    for (int index = 0; index < 3; ++index) {
        auto result = replies[index].result ();
        ASSERT_TRUE (result);
        messaging::message_parts_t parts (std::move (result.value ()));
        auto header = messaging::envelope_codec_t{}.decode_header (parts);
        auto body = messaging::envelope_codec_t{}.decode_body (parts);
        ASSERT_TRUE (header);
        ASSERT_TRUE (body);
        EXPECT_EQ (std::to_string (index + 1), header.value ().correlation_id);
        EXPECT_EQ (std::to_string (index + 101), body.value ().to_string ());
    }
    if (async_filter) {
        std::unique_lock lock (handler.mutex);
        ASSERT_TRUE (handler.changed.wait_for (
          lock, 2s, [&] { return handler.scopes_destroyed.load () == 3; }));
        EXPECT_EQ (3, handler.scopes_created.load ());
        EXPECT_EQ (3, handler.scopes_destroyed.load ());
    }
    EXPECT_TRUE (executor->drain_until (std::chrono::steady_clock::now () + 2s));
}

TEST (ManualDispatchOwner, NativeBatchPostsOneOwnerAndStartsPendingPipelinesInFifo)
{
    check_native_fifo (true, false);
}

TEST (ManualDispatchOwner, AsyncFilterRetainsPermitAndScopeWhileLaterPipelinesEnter)
{
    check_native_fifo (true, true);
}

TEST (ManualDispatchOwner, NativeMultiworkerDrainStartsPipelinesInFifo)
{
    check_native_fifo (false, true);
}

TEST (ManualDispatchOwner, RejectedOwnerSubmissionKeepsShutdownDropAndReturnsAcceptedPermit)
{
    check_native_fifo (true, false, true);
}

TEST (ManualDispatchOwner, NativeFailureMatrixSettlesPermitsScopesAndOriginalReplies)
{
    using namespace std::chrono_literals;
    native_manual_fixture_t fixture (std::make_unique<fw::test::manual_internal_scheduler_t> (),
                                     true);
    auto &state = *fixture.handler;
    state.fault_matrix = true;
    // Transport liveness §3 requires the admitted raw peer to receive and ACK
    // the initial probe, as in the existing ClientServer native-wire fixture.
    namespace protocol = fw::runtime::protocol;
    fixture.start ();
    auto hello = zlink::message_t::from (protocol::encode_client_server_client_admission (
      protocol::command::hello,
      {"manual", "default",
       static_cast<std::uint32_t> (fw::runtime::dispatch_limits::receive_batch_bytes)}));
    auto admission =
      native_result (fixture.client->request ().message (hello).timeout (2s).async ().reply);
    auto admitted = admission.result ();
    ASSERT_TRUE (admitted);
    ASSERT_EQ (1u, admitted.value ().size ());
    EXPECT_EQ ("manual",
               protocol::decode_client_server_server_admission (
                 protocol::message_bytes (admitted.value ().front ()), protocol::command::admit)
                 .channel_name);
    fixture.client->options ().recv_timeout (2s);
    zlink::received_t initial_probe;
    ASSERT_EQ (0, fixture.client->recv (initial_probe));
    ASSERT_EQ (1u, initial_probe.parts ().size ());
    auto incoming =
      protocol::decode_liveness (protocol::message_bytes (initial_probe.parts ().front ()));
    ASSERT_EQ (protocol::command::livenessProbe, incoming.kind);
    ASSERT_NE (0u, incoming.probe_id);
    auto initial_ack = zlink::message_t::from (
      protocol::encode_liveness (protocol::command::livenessAck, incoming.probe_id));
    fixture.client->send ().message (initial_ack).submit ();
    for (int value = 1; value <= 6; ++value)
        fixture.request (value, value == 2 ? "missing" : "request",
                         value == 3 ? std::optional<std::string> ("invalid-number") : std::nullopt);
    // A liveness request follows DATA on the same native route. Its reply
    // proves receive progress while application scheduling is held.
    constexpr std::uint64_t marker_probe_id = 1;
    auto marker = zlink::message_t::from (
      protocol::encode_liveness (protocol::command::livenessProbe, marker_probe_id));
    auto acknowledged =
      native_result (fixture.client->request ().message (marker).timeout (2s).async ().reply);
    auto marker_reply = acknowledged.result ();
    ASSERT_TRUE (marker_reply);
    ASSERT_EQ (1u, marker_reply.value ().size ());
    auto marker_ack =
      protocol::decode_liveness (protocol::message_bytes (marker_reply.value ().front ()));
    ASSERT_EQ (protocol::command::livenessAck, marker_ack.kind);
    ASSERT_EQ (marker_probe_id, marker_ack.probe_id);
    ASSERT_EQ (6u, fixture.jobs->snapshot ().queued_application_jobs);
    ASSERT_EQ (6u, fixture.server->pending_messages ());
    ASSERT_TRUE (fixture.held->wait_for_pending_work (1, 2s))
      << "owner submission did not become pending";
    ASSERT_EQ (1u, fixture.held->pending_work ());
    fixture.held->run_all ();
    EXPECT_EQ ((std::vector<int>{1, 3, 4, 5, 6}), state.pipeline_entries);
    EXPECT_EQ ((std::vector<int>{4, 6}), state.entries);
    EXPECT_EQ (1u, fixture.jobs->snapshot ().queued_application_jobs);
    EXPECT_EQ (5, state.scopes_created.load ());
    EXPECT_EQ (4, state.scopes_destroyed.load ());
    EXPECT_EQ (0u, fixture.server->pending_messages ());
    EXPECT_FALSE (fixture.executor->drain_until (std::chrono::steady_clock::now ()));
    state.release_filter.complete (fw::result_t<void>::failure (
      fw::framework_error_kind_t::rejected, "native manual asynchronous filter failure"));
    for (int index = 0; index != 6; ++index) {
        auto reply = fixture.replies[index].result ();
        if (!reply)
            fixture.dump_diagnostics ();
        ASSERT_TRUE (reply) << "native request=" << index + 1 << " error="
                            << (reply.error () ? reply.error ()->what () : "no error object");
        messaging::message_parts_t parts (std::move (reply.value ()));
        auto header = messaging::envelope_codec_t{}.decode_header (parts);
        ASSERT_TRUE (header);
        EXPECT_EQ (std::to_string (index + 1), header.value ().correlation_id);
        EXPECT_EQ (index == 5 ? messaging::message_kind_t::response
                              : messaging::message_kind_t::error,
                   header.value ().kind);
        if (index != 5)
            EXPECT_TRUE (header.value ().error_code.has_value ());
    }
    EXPECT_TRUE (fixture.executor->drain_until (std::chrono::steady_clock::now () + 2s));
    EXPECT_EQ (0u, fixture.jobs->snapshot ().queued_application_jobs);
    EXPECT_EQ (5, state.scopes_destroyed.load ());
    {
        std::unique_lock lock (fixture.diagnostics->mutex);
        ASSERT_TRUE (fixture.diagnostics->changed.wait_for (
          lock, 2s, [&] { return fixture.diagnostics->errors.size () == 5; }));
        ASSERT_EQ (5u, fixture.diagnostics->errors.size ());
        for (const auto &error : fixture.diagnostics->errors) {
            EXPECT_TRUE (error.error_type.has_value () && !error.error_type->empty ());
            EXPECT_TRUE (error.error_message.has_value () && !error.error_message->empty ());
        }
    }
}

TEST (ManualDispatchOwner, ScopeCreationFailureReturnsEveryAcceptedPrefixPermit)
{
    using namespace std::chrono_literals;
    native_manual_fixture_t fixture (std::make_unique<fw::test::manual_internal_scheduler_t> (),
                                     true);
    for (int value = 1; value <= 3; ++value)
        fixture.request (value);
    fixture.start ();
    const auto watchdog = std::chrono::steady_clock::now () + 2s;
    while ((fixture.server->pending_messages () != 3 || fixture.held->pending_work () != 1)
           && std::chrono::steady_clock::now () < watchdog)
        std::this_thread::yield ();
    ASSERT_EQ (3u, fixture.server->pending_messages ());
    ASSERT_EQ (1u, fixture.held->pending_work ());
    fixture.provider.close ();
    fixture.held->run_all ();
    EXPECT_EQ (0u, fixture.jobs->snapshot ().queued_application_jobs);
    EXPECT_EQ (0u, fixture.server->pending_messages ());
    EXPECT_TRUE (fixture.handler->entries.empty ());
    EXPECT_EQ (0, fixture.handler->scopes_created.load ());
    EXPECT_TRUE (fixture.executor->drained ());
    std::unique_lock lock (fixture.diagnostics->mutex);
    ASSERT_TRUE (fixture.diagnostics->changed.wait_for (
      lock, 2s, [&] { return fixture.diagnostics->errors.size () == 3; }));
    ASSERT_EQ (3u, fixture.diagnostics->errors.size ());
    for (const auto &error : fixture.diagnostics->errors)
        EXPECT_EQ (fw::dispatch_error_reason_t::handler_exception, error.reason);
}

TEST (ManualDispatchOwner, ReplyFailureClosesPendingScopeAndPhysicalTerminalOnce)
{
    using namespace std::chrono_literals;
    native_manual_fixture_t fixture (std::make_unique<fw::test::manual_internal_scheduler_t> (),
                                     true);
    fixture.handler->release_filter.complete (fw::result_t<void>::success ());
    fixture.request (1);
    fixture.start ();
    const auto accepted_watchdog = std::chrono::steady_clock::now () + 2s;
    while ((fixture.server->pending_messages () != 1 || fixture.held->pending_work () != 1)
           && std::chrono::steady_clock::now () < accepted_watchdog)
        std::this_thread::yield ();
    ASSERT_EQ (1u, fixture.server->pending_messages ());
    ASSERT_EQ (1u, fixture.held->pending_work ());
    fixture.held->run_all ();
    ASSERT_EQ (1u, fixture.handler->entries.size ());
    ASSERT_EQ (1u, fixture.server->peer_count ());
    fixture.completions.reset ();
    fixture.client->close ();
    const auto disconnected_watchdog = std::chrono::steady_clock::now () + 2s;
    while (fixture.server->peer_count () != 0
           && std::chrono::steady_clock::now () < disconnected_watchdog)
        std::this_thread::yield ();
    ASSERT_EQ (0u, fixture.server->peer_count ());
    EXPECT_EQ (0, fixture.handler->scopes_destroyed.load ());
    EXPECT_FALSE (fixture.executor->drain_until (std::chrono::steady_clock::now ()));
    fixture.handler->release_first.complete (fw::result_t<void>::success ());
    ASSERT_TRUE (fixture.executor->drain_until (std::chrono::steady_clock::now () + 2s));
    EXPECT_EQ (1, fixture.handler->scopes_created.load ());
    EXPECT_EQ (1, fixture.handler->scopes_destroyed.load ());
    EXPECT_EQ (0u, fixture.jobs->snapshot ().queued_application_jobs);
    EXPECT_EQ (0u, fixture.server->pending_messages ());
    std::unique_lock lock (fixture.diagnostics->mutex);
    ASSERT_TRUE (fixture.diagnostics->changed.wait_for (
      lock, 2s, [&] { return fixture.diagnostics->errors.size () == 1; }));
    ASSERT_EQ (1u, fixture.diagnostics->errors.size ());
    EXPECT_EQ (fw::dispatch_error_reason_t::unexpected_reply,
               fixture.diagnostics->errors.front ().reason);
    const auto &error = fixture.diagnostics->errors.front ();
    EXPECT_TRUE (error.error_type.has_value () && !error.error_type->empty ());
    EXPECT_TRUE (error.error_message.has_value () && !error.error_message->empty ());
}

TEST (ManualDispatchOwner, ShutdownDrainsAcceptedPrefixesBeforePendingPhysicalTerminal)
{
    using namespace std::chrono_literals;
    constexpr auto count = fw::runtime::dispatch_limits::receive_batch_messages + 1;
    native_manual_fixture_t fixture (
      std::make_unique<fw::runtime::offload_executor_t> (1, "manual-shutdown-prefix"), true,
      static_cast<std::uint32_t> (2 * fw::runtime::dispatch_limits::receive_batch_messages));
    std::promise<void> release_prefix;
    fixture.handler->prefix_block = release_prefix.get_future ().share ();
    fixture.handler->release_filter.complete (fw::result_t<void>::success ());
    fw::test::scope_exit_t unblock_prefix ([&] { release_prefix.set_value (); });
    for (int value = 1; value <= static_cast<int> (count); ++value)
        fixture.request (value);
    fixture.start ();
    {
        std::unique_lock lock (fixture.handler->mutex);
        ASSERT_TRUE (fixture.handler->changed.wait_for (
          lock, 2s, [&] { return fixture.handler->pipeline_entries.size () == 1; }));
    }
    const auto accepted_watchdog = std::chrono::steady_clock::now () + 2s;
    while (fixture.jobs->snapshot ().queued_application_jobs != count
           && std::chrono::steady_clock::now () < accepted_watchdog)
        std::this_thread::yield ();
    ASSERT_EQ (count, fixture.jobs->snapshot ().queued_application_jobs);
    fixture.stopping.store (true, std::memory_order_release);
    fixture.server->signal_activity ();
    unblock_prefix.run ();
    {
        std::unique_lock lock (fixture.handler->mutex);
        ASSERT_TRUE (fixture.handler->changed.wait_for (lock, 2s, [&] {
            return fixture.handler->entries.size () == count
                   && fixture.handler->scopes_destroyed.load () == static_cast<int> (count - 1);
        }));
        for (std::size_t index = 0; index != count; ++index)
            EXPECT_EQ (static_cast<int> (index + 1), fixture.handler->pipeline_entries[index]);
    }
    EXPECT_EQ (0u, fixture.server->pending_messages ());
    EXPECT_EQ (0u, fixture.jobs->snapshot ().queued_application_jobs);
    EXPECT_EQ (std::future_status::timeout, fixture.stopped.wait_for (0ms));
    fixture.handler->release_first.complete (fw::result_t<void>::success ());
    ASSERT_EQ (std::future_status::ready, fixture.stopped.wait_for (2s));
    fixture.stopped.get ();
    fixture.receiver.join ();
    EXPECT_EQ (static_cast<int> (count), fixture.handler->scopes_destroyed.load ());
    EXPECT_TRUE (fixture.executor->drained ());
    for (std::size_t index = 0; index != count; ++index) {
        auto reply = fixture.replies[index].result ();
        ASSERT_TRUE (reply);
        messaging::message_parts_t parts (std::move (reply.value ()));
        auto header = messaging::envelope_codec_t{}.decode_header (parts);
        ASSERT_TRUE (header);
        EXPECT_EQ (std::to_string (index + 1), header.value ().correlation_id);
    }
}
