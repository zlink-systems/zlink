/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/json.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "runtime/mesh/mesh_node_host_service.hpp"
#include <zlink/framework.hpp>
#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/channels/route_packet_dispatcher.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "../support/allocation_counter.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/scope_exit.hpp"
#include "runtime/dispatch/offload_executor.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <tuple>

namespace
{
using namespace zlink::framework;

struct suspended_route_state_t
{
    task_completion_source_t<void> handler_release;
    task_completion_source_t<void> filter_release;
    std::promise<void> first_pipeline;
    std::atomic_int instructions{0};
    std::atomic_int terminals{0};
    std::atomic_int created{0};
    std::atomic_int destroyed{0};
    bool fail_handler = false;
};

struct suspended_route_handler_t
{
    explicit suspended_route_handler_t (suspended_route_state_t &state) : state (state)
    {
        ++state.created;
    }
    ~suspended_route_handler_t () { ++state.destroyed; }

    suspended_route_state_t &state;

    task_t<void> send (const int &value, const route_message_context_t &context)
    {
        ++state.instructions;
        if (value == 1)
            co_await state.handler_release.task ();
        EXPECT_EQ (value, context.metadata.find ("value").has_value ()
                            ? std::stoi (std::string (*context.metadata.find ("value")))
                            : 0);
        ++state.terminals;
        if (state.fail_handler)
            throw std::runtime_error ("route handler failed");
    }

    task_t<int> request (const int &value, const route_message_context_t &context)
    {
        co_await send (value, context);
        co_return value;
    }
};

// The same fixture accepts the old synchronous private result and its asynchronous
// replacement, so OLD must fail on behavior rather than on a signature mismatch.
template <typename TOperation, typename TComplete>
void observe_route_operation (TOperation &operation, TComplete complete)
{
    if constexpr (requires { operation.await_ready (); })
        detail::observe_task_terminal (operation, std::move (complete));
    else
        complete (operation);
}

TEST (RouteHandlerBoundary, SuspendedRegisteredInvocationReturnsTheSingleWorker)
{
    using namespace std::chrono_literals;
    for (bool request : {false, true}) {
        for (bool suspend_filter : {false, true}) {
            SCOPED_TRACE (std::string (request ? "request" : "send")
                          + (suspend_filter ? "/filter" : "/handler"));
            suspended_route_state_t state;
            service_collection_t services;
            services.add_factory<suspended_route_handler_t> ([&] (service_provider_t &) {
                return std::make_unique<suspended_route_handler_t> (state);
            });
            auto provider = services.build_provider ();
            serializer_registry_t serializers;
            handler_registry_t filters;
            filters.add_filter ([&] (service_provider_t &, serializer_registry_t &,
                                     const handler_filter_context_t &context,
                                     handler_next_t next) -> task_t<void> {
                if (context.metadata.find ("value") == std::optional<std::string_view> ("1")) {
                    state.first_pipeline.set_value ();
                    if (suspend_filter)
                        co_await state.filter_release.task ();
                }
                co_await next ();
            });
            detail::route_handler_registry_t handlers;
            handlers.on_send<suspended_route_handler_t, int> ("mesh", "packet",
                                                              &suspended_route_handler_t::send);
            handlers.on_request<suspended_route_handler_t, int, int> (
              "mesh", "packet", &suspended_route_handler_t::request);
            std::atomic_int permits{0};
            std::atomic_int completions{0};
            std::promise<void> all_completed;
            auto completed = all_completed.get_future ();
            std::promise<void> worker_marker;
            auto entered = state.first_pipeline.get_future ();
            auto marker = worker_marker.get_future ();
            runtime::offload_executor_t worker (1, "route-async-proof");
            test::scope_exit_t cleanup ([&] {
                state.filter_release.complete (result_t<void>::success ());
                state.handler_release.complete (result_t<void>::success ());
                worker.drain ();
                EXPECT_EQ (std::future_status::ready, completed.wait_for (1s));
            });
            const auto dispatch = [&] (int value) {
                detail::no_route_internal_packet_dispatcher_t internal;
                detail::route_packet_dispatcher_t dispatcher (
                  "mesh", provider, serializers, handlers, internal, {}, &filters,
                  handler_dispatch_kind_t::node_direct_send,
                  handler_dispatch_kind_t::node_direct_request, [&] { ++permits; });
                runtime::messaging::envelope_header_t header;
                header.kind = request ? runtime::messaging::message_kind_t::request
                                      : runtime::messaging::message_kind_t::command;
                header.message_name = "packet";
                header.content_type = "application/json";
                header.metadata["value"] = std::to_string (value);
                auto parts = runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (
                  header, zlink::message_t::from (std::to_string (value)));
                auto operation = dispatcher.dispatch ({zlink::routing_id_t::from ("source"),
                                                       std::uint64_t (value),
                                                       std::move (parts),
                                                       {}},
                                                      std::move (header));
                observe_route_operation (operation, [&, value] (const auto &result) {
                    EXPECT_TRUE (result);
                    if (result && request) {
                        EXPECT_TRUE (result.value ().has_value ());
                        if (result.value ()) {
                            EXPECT_EQ (std::optional<std::uint64_t> (value),
                                       result.value ()->request_seq);
                            EXPECT_EQ ("source", result.value ()->target_node_rid.to_string ());
                            const auto reply_body =
                              runtime::messaging::envelope_codec_t{}.decode_body (
                                result.value ()->parts);
                            EXPECT_TRUE (reply_body);
                            if (reply_body)
                                EXPECT_EQ (std::to_string (value),
                                           reply_body.value ().to_string ());
                        }
                    }
                    if (++completions == 2)
                        all_completed.set_value ();
                });
            };
            ASSERT_TRUE (worker.try_submit_internal ([&] { dispatch (1); }));
            ASSERT_EQ (std::future_status::ready, entered.wait_for (1s));
            ASSERT_TRUE (worker.try_submit_internal ([&] {
                dispatch (2);
                worker_marker.set_value ();
            }));
            EXPECT_EQ (std::future_status::ready, marker.wait_for (1s))
              << "the only application worker waited for the first invocation terminal";
            if (marker.wait_for (0s) == std::future_status::ready) {
                EXPECT_EQ (suspend_filter ? 1 : 2, permits.load ());
                EXPECT_EQ (1, state.terminals.load ());
                EXPECT_EQ (1, completions.load ());
                EXPECT_EQ (suspend_filter ? 1 : 2, state.created.load ());
                EXPECT_EQ (1, state.destroyed.load ());
            }
            cleanup.run ();
            EXPECT_EQ (2, permits.load ());
            EXPECT_EQ (2, state.instructions.load ());
            EXPECT_EQ (2, state.terminals.load ());
            EXPECT_EQ (2, completions.load ());
            EXPECT_EQ (2, state.created.load ());
            EXPECT_EQ (2, state.destroyed.load ());
        }
    }
}

TEST (RouteHandlerBoundary, PendingMeshReplyOwnsNativeStorageAndScopeUntilTerminal)
{
    namespace host = runtime::host;
    namespace msg = runtime::messaging;
    for (const bool request : {false, true}) {
        for (const bool fail_handler : {false, true}) {
            for (const bool fail_reply : {false, true}) {
                if (!request && fail_reply)
                    continue;
                SCOPED_TRACE (std::to_string (request) + ":" + std::to_string (fail_handler) + ":"
                              + std::to_string (fail_reply));
                suspended_route_state_t state;
                state.fail_handler = fail_handler;
                service_collection_t services;
                services.add_factory<suspended_route_handler_t> ([&] (service_provider_t &) {
                    return std::make_unique<suspended_route_handler_t> (state);
                });
                auto provider = services.build_provider ();
                serializer_registry_t serializers;
                detail::route_handler_registry_t handlers;
                handlers.on_send<suspended_route_handler_t, int> ("mesh", "packet",
                                                                  &suspended_route_handler_t::send);
                handlers.on_request<suspended_route_handler_t, int, int> (
                  "mesh", "packet", &suspended_route_handler_t::request);
                handler_registry_t filters;
                auto reply_host =
                  std::make_shared<host::public_host_runtime_t> (host::host_options_t{
                    .mesh = {.descriptor = {.mesh_name = "mesh",
                                            .node_routing_id =
                                              zlink::routing_id_t::from ("reply").to_bytes (),
                                            .lifecycle_generation = 1,
                                            .descriptor_revision = 1,
                                            .advertised_endpoint = "tcp://127.0.0.1:0"},
                             .runtime_failures =
                               std::make_shared<runtime::runtime_failure_collector_t> ()}});
                std::weak_ptr<runtime::mesh::service_mailbox_record_t> request_storage;
                std::weak_ptr<int> registration_storage;
                int replies = 0;
                int permits = 0;
                std::vector<zlink::message_t> reply_parts;
                std::optional<task_t<std::optional<detail::route_dispatch_reply_t>>> pending;
                test::scope_exit_t cleanup ([&] {
                    state.handler_release.complete (result_t<void>::success ());
                    if (pending)
                        (void) pending->result ();
                });
                {
                    auto registration = std::make_shared<int> (7);
                    registration_storage = registration;
                    detail::mesh_record_dispatcher_t dispatcher (
                      provider, serializers, handlers, filters, {}, [&] { ++permits; },
                      registration);
                    host::receive_record_t record;
                    record.kind =
                      request ? host::record_kind_t::node_request : host::record_kind_t::node_send;
                    record.channel_name = "mesh";
                    record.source_node_rid = zlink::routing_id_t::from ("source");
                    if (request) {
                        record.reply_token.host = reply_host;
                        record.reply_token.request =
                          std::make_shared<runtime::mesh::service_mailbox_record_t> ();
                        request_storage = record.reply_token.request;
                        record.reply_token.local_reply = [&] (const auto &parts) {
                            EXPECT_FALSE (request_storage.expired ());
                            EXPECT_FALSE (registration_storage.expired ());
                            EXPECT_EQ (0, state.destroyed);
                            ++replies;
                            reply_parts = parts;
                            return !fail_reply;
                        };
                    }
                    msg::envelope_header_t header;
                    header.kind =
                      request ? msg::message_kind_t::request : msg::message_kind_t::command;
                    header.channel_name = "mesh";
                    header.message_name = "packet";
                    header.content_type = "application/json";
                    header.correlation_id = "pending-route-storage";
                    header.metadata["value"] = "1";
                    auto parts = msg::envelope_codec_t{}
                                   .encode_raw_body_parts (header, zlink::message_t::from ("1"))
                                   .take_items ();
                    pending.emplace (dispatcher.dispatch (record, std::move (parts), header));
                    ASSERT_FALSE (pending->await_ready ());
                    EXPECT_EQ (1, permits);
                    EXPECT_EQ (1, state.created);
                    EXPECT_EQ (0, state.destroyed);
                }
                EXPECT_FALSE (registration_storage.expired ());
                if (request)
                    EXPECT_FALSE (request_storage.expired ());
                EXPECT_EQ (0, replies);
                state.handler_release.complete (result_t<void>::success ());
                const auto result = pending->result ();
                EXPECT_EQ (request ? !fail_reply : !fail_handler, static_cast<bool> (result));
                EXPECT_EQ (1, state.terminals);
                EXPECT_EQ (1, state.destroyed);
                EXPECT_EQ (request ? 1 : 0, replies);
                EXPECT_TRUE (request_storage.expired ());
                EXPECT_TRUE (registration_storage.expired ());
                if (request) {
                    auto header =
                      msg::envelope_codec_t{}.decode_header (msg::message_parts_t (reply_parts));
                    ASSERT_TRUE (header);
                    EXPECT_EQ ("pending-route-storage", header.value ().correlation_id);
                    EXPECT_EQ (fail_handler ? msg::message_kind_t::error
                                            : msg::message_kind_t::response,
                               header.value ().kind);
                    if (fail_handler)
                        EXPECT_EQ ("internal_failure", header.value ().error_code);
                    else {
                        auto body =
                          msg::envelope_codec_t{}.decode_body (msg::message_parts_t (reply_parts));
                        ASSERT_TRUE (body);
                        EXPECT_EQ ("1", body.value ().to_string ());
                    }
                }
            }
        }
    }
}

// App owns the catalogs and this hosted adapter owns the real MeshNode host.
// Initialization follows App readiness so the existing Location owner is usable.
class route_host_fixture_service_t final : public hosted_service_t,
                                           public runtime::hosted_service_lifecycle_t
{
  public:
    route_host_fixture_service_t (app_t &app,
                                  std::shared_ptr<detail::mesh_node_builder_state_t> registration) :
        app (app), registration (std::move (registration))
    {
    }

    task_t<void> start (service_provider_t &value) override
    {
        services = &value;
        co_return;
    }

    void initialize ()
    {
        registration->core_context =
          detail::zlink_builder_access_t::shared_core_context (app.advanced ().zlink ());
        host = std::make_unique<runtime::mesh_node_host_service_t> (
          failures, std::vector{registration}, services->get_required<serializer_registry_t> (),
          app.advanced ().handlers ());
        host->start (*services).result ().value ();
    }

    void stop () noexcept override
    {
        if (host)
            host->stop ();
    }
    void request_stop () noexcept override
    {
        if (host)
            host->request_stop ();
    }
    void seal_application_dispatch () noexcept override
    {
        if (host)
            host->seal_application_dispatch ();
    }
    bool wait_for_accepted_callbacks_until (
      std::chrono::steady_clock::time_point deadline) noexcept override
    {
        return !host || host->wait_for_accepted_callbacks_until (deadline);
    }
    bool publish_descriptor_state (framework_runtime_state_t state) noexcept override
    {
        return !host || host->publish_descriptor_state (state);
    }

    app_t &app;
    std::shared_ptr<detail::mesh_node_builder_state_t> registration;
    service_provider_t *services = nullptr;
    std::shared_ptr<runtime::runtime_failure_collector_t> failures =
      std::make_shared<runtime::runtime_failure_collector_t> ();
    std::unique_ptr<runtime::mesh_node_host_service_t> host;
};

TEST (RouteHandlerBoundary, HostedOrdinaryHandoffAllowsSameAndSubsequentClaims)
{
    using namespace std::chrono_literals;
    namespace host = runtime::host;
    namespace mesh = runtime::mesh;
    suspended_route_state_t state;
    auto app = app_t::create ();
    app.advanced ().services ().add_factory<suspended_route_handler_t> (
      [&] (service_provider_t &) { return std::make_unique<suspended_route_handler_t> (state); });
    auto &options = app.add_zlink_framework ();
    options.add_location_store (std::make_shared<runtime::in_memory_location_store_t> ());
    options.add_route_mesh ("route-proof-infrastructure")
      .listen ("tcp://127.0.0.1:0")
      .set_routing_id (zlink::routing_id_t::from ("proof-infrastructure"));
    auto registration =
      test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("route-proof");
    registration->routing_id = zlink::routing_id_t::from ("proof-owner");
    registration->has_node_direct_handler = true;
    registration->handlers.on_send<suspended_route_handler_t, int> (
      "route-proof", "packet", &suspended_route_handler_t::send);
    auto service = std::make_unique<route_host_fixture_service_t> (app, registration);
    auto *view = service.get ();
    app.add_hosted_service (std::move (service));
    char program[] = "route-owner-proof";
    char *arguments[] = {program, nullptr};
    int run_exit = -1;
    std::atomic_int permits{0};
    std::thread run ([&] { run_exit = app.run (1, arguments); });
    test::scope_exit_t cleanup ([&] {
        state.handler_release.complete (result_t<void>::success ());
        app.request_stop ();
        run.join ();
    });
    const auto wait = [] (const auto &predicate, auto budget) {
        const auto deadline = std::chrono::steady_clock::now () + budget;
        while (!predicate () && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        return predicate ();
    };
    ASSERT_TRUE (wait ([&] { return app.is_ready (); }, 2s));
    ASSERT_NO_THROW (view->initialize ());
    auto node = view->host->nodes ().front ();
    auto &mailbox = node->native_node ().transport ().mailbox ();
    const auto enqueue = [&] (int value) {
        runtime::messaging::envelope_header_t header;
        header.kind = runtime::messaging::message_kind_t::command;
        header.channel_name = "route-proof";
        header.message_name = "packet";
        header.content_type = "application/json";
        header.metadata["value"] = std::to_string (value);
        host::ready_record_t owner;
        owner.owner_kind = host::owner_kind_t::node;
        owner.domain = host::ready_domain_t::application;
        host::receive_record_t record;
        record.kind = host::record_kind_t::node_send;
        record.domain = host::ready_domain_t::application;
        record.source_node_rid = zlink::routing_id_t::from ("source");
        auto parts =
          runtime::messaging::envelope_codec_t{}
            .encode_raw_body_parts (header, zlink::message_t::from (std::to_string (value)))
            .take_items ();
        mesh::service_mailbox_record_t queued;
        queued.owner = mesh::service_mailbox_t::application_owner (owner);
        queued.domain = mesh::service_mailbox_domain_t::application;
        queued.before_application_handler = [&] { ++permits; };
        queued.application = std::make_shared<host::local_application_dispatch_t> (
          host::local_application_dispatch_t{owner, record, std::move (parts)});
        return mailbox.try_enqueue (std::move (queued));
    };
    mailbox.begin_application_receive_turn ();
    const bool first = enqueue (1);
    const bool second = enqueue (2);
    mailbox.end_application_receive_turn ();
    ASSERT_TRUE (first && second);
    ASSERT_TRUE (wait (
      [&] { return state.instructions == 2 && state.terminals == 1 && state.destroyed == 1; }, 1s))
      << "the bounded claim did not enter r2 while r1 was pending";
    EXPECT_EQ (2, permits);
    EXPECT_EQ (1, state.destroyed);
    ASSERT_TRUE (wait (
      [&] { return mailbox.pending_messages (mesh::service_mailbox_domain_t::application) == 0; },
      1s))
      << "ordinary prefix credit remained coupled to r1 physical completion";
    ASSERT_TRUE (enqueue (3));
    ASSERT_TRUE (wait ([&] { return state.instructions == 3 && state.terminals == 2; }, 1s))
      << "a new claim could not enter after the first claim handed off r1";
    EXPECT_EQ (3, permits);
    EXPECT_FALSE (
      view->host->wait_for_accepted_callbacks_until (std::chrono::steady_clock::now ()));
    state.handler_release.complete (result_t<void>::success ());
    ASSERT_TRUE (wait ([&] { return state.terminals == 3 && state.destroyed == 3; }, 1s));
    EXPECT_TRUE (
      view->host->wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));
    node.reset ();
    cleanup.run ();
    EXPECT_EQ (0, run_exit);
    EXPECT_NO_THROW (view->failures->rethrow_if_failed ());
}

// A native provider may finish after Framework wait cancellation. This gate
// deliberately has no task wait registration; the fixture always releases it.
struct physical_route_gate_t
{
    std::coroutine_handle<> continuation;
    bool await_ready () const noexcept { return false; }
    void await_suspend (std::coroutine_handle<> value) noexcept { continuation = value; }
    void await_resume () const noexcept {}
    void release ()
    {
        if (auto value = std::exchange (continuation, {}))
            value.resume ();
    }
};

struct physical_route_state_t
{
    physical_route_gate_t gate;
    int destroyed = 0;
    int physical_cleanup = 0;
};

struct physical_route_handler_t
{
    physical_route_state_t &state;
    ~physical_route_handler_t () { ++state.destroyed; }
    task_t<void> handle (const int &, const route_message_context_t &)
    {
        // Keep this proof safe even in BAD: after suspension only the fixture
        // state is accessed, so early scope disposal is reported without UAF.
        auto *physical = &state;
        co_await physical->gate;
        ++physical->physical_cleanup;
    }
};

TEST (RouteHandlerBoundary, CancellationCannotReplacePhysicalHandlerTerminal)
{
    physical_route_state_t state;
    service_collection_t services;
    services.add_factory<physical_route_handler_t> ([&] (service_provider_t &) {
        return std::unique_ptr<physical_route_handler_t> (new physical_route_handler_t{state});
    });
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    handler_registry_t filters;
    detail::route_handler_registry_t handlers;
    handlers.on_send<physical_route_handler_t, int> ("mesh", "physical",
                                                     &physical_route_handler_t::handle);
    route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
    context.content_type = "application/json";
    detail::ambient_context_scope_t infrastructure (nullptr, nullptr, {}, false, true);
    auto pending = detail::route_handler_invoker_t{}.invoke (
      *handlers.resolve ("mesh", runtime::messaging::message_kind_t::command, "physical"), filters,
      handler_dispatch_kind_t::node_direct_send, provider, serializers,
      zlink::message_t::from ("1"), std::move (context));
    test::scope_exit_t cleanup ([&] {
        state.gate.release ();
        (void) pending.result ();
    });
    ASSERT_FALSE (pending.await_ready ());
    detail::cancel_handler_waits (filters);
    EXPECT_FALSE (pending.await_ready ())
      << "logical cancellation ended the invocation before the native handler cleanup";
    EXPECT_EQ (0, state.destroyed);
    EXPECT_EQ (0, state.physical_cleanup);
    cleanup.run ();
    EXPECT_EQ (1, state.destroyed);
    EXPECT_EQ (1, state.physical_cleanup);
}

struct host_stop_route_state_t
{
    struct gate_t
    {
        std::mutex mutex;
        bool released = false;
        std::coroutine_handle<> continuation;
        std::promise<void> entered;

        bool await_ready () const noexcept { return false; }
        bool await_suspend (std::coroutine_handle<> value)
        {
            std::lock_guard lock (mutex);
            entered.set_value ();
            if (released)
                return false;
            continuation = value;
            return true;
        }
        void await_resume () const noexcept {}
        void release ()
        {
            std::coroutine_handle<> value;
            {
                std::lock_guard lock (mutex);
                released = true;
                value = std::exchange (continuation, {});
            }
            if (value)
                value.resume ();
        }
    } gate;
    std::atomic_int physical_terminal{0};
    std::atomic_int destroyed{0};
};

struct host_stop_route_handler_t
{
    host_stop_route_state_t &state;
    ~host_stop_route_handler_t () { ++state.destroyed; }
    task_t<void> handle (const int &, const route_message_context_t &)
    {
        auto *physical = &state;
        co_await physical->gate;
        ++physical->physical_terminal;
    }
};

TEST (RouteHandlerBoundary, HostStopWaitsForPhysicalOrdinaryHandlerTerminal)
{
    using namespace std::chrono_literals;
    namespace host = runtime::host;
    namespace mesh = runtime::mesh;
    host_stop_route_state_t state;
    auto entered = state.gate.entered.get_future ();
    auto app = app_t::create ();
    app.advanced ().services ().add_factory<host_stop_route_handler_t> ([&] (service_provider_t &) {
        return std::unique_ptr<host_stop_route_handler_t> (new host_stop_route_handler_t{state});
    });
    auto &options = app.add_zlink_framework ();
    options.add_location_store (std::make_shared<runtime::in_memory_location_store_t> ());
    options.add_route_mesh ("route-stop-infrastructure")
      .listen ("tcp://127.0.0.1:0")
      .set_routing_id (zlink::routing_id_t::from ("stop-infrastructure"));
    auto registration =
      test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("route-stop-proof");
    registration->routing_id = zlink::routing_id_t::from ("stop-owner");
    registration->has_node_direct_handler = true;
    registration->handlers.on_send<host_stop_route_handler_t, int> (
      "route-stop-proof", "packet", &host_stop_route_handler_t::handle);
    auto service = std::make_unique<route_host_fixture_service_t> (app, registration);
    auto *view = service.get ();
    app.add_hosted_service (std::move (service));
    char program[] = "route-stop-proof";
    char *arguments[] = {program, nullptr};
    int run_exit = -1;
    std::shared_ptr<detail::mesh_node_runtime_t> node;
    std::promise<void> stop_started;
    std::promise<void> stop_completed;
    auto started = stop_started.get_future ();
    auto completed = stop_completed.get_future ();
    std::thread stop_thread;
    std::thread run ([&] { run_exit = app.run (1, arguments); });
    test::scope_exit_t cleanup ([&] {
        // Release also covers an entry that arrives after an assertion fails.
        state.gate.release ();
        if (stop_thread.joinable ())
            stop_thread.join ();
        app.request_stop ();
        run.join ();
    });
    const auto ready_deadline = std::chrono::steady_clock::now () + 2s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < ready_deadline)
        std::this_thread::yield ();
    ASSERT_TRUE (app.is_ready ());
    ASSERT_NO_THROW (view->initialize ());
    node = view->host->nodes ().front ();
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::command;
    header.channel_name = "route-stop-proof";
    header.message_name = "packet";
    header.content_type = "application/json";
    host::ready_record_t owner;
    owner.owner_kind = host::owner_kind_t::node;
    owner.domain = host::ready_domain_t::application;
    host::receive_record_t record;
    record.kind = host::record_kind_t::node_send;
    record.domain = host::ready_domain_t::application;
    record.source_node_rid = zlink::routing_id_t::from ("source");
    auto parts = runtime::messaging::envelope_codec_t{}
                   .encode_raw_body_parts (header, zlink::message_t::from ("1"))
                   .take_items ();
    mesh::service_mailbox_record_t queued;
    queued.owner = mesh::service_mailbox_t::application_owner (owner);
    queued.domain = mesh::service_mailbox_domain_t::application;
    queued.before_application_handler = [] {};
    queued.application = std::make_shared<host::local_application_dispatch_t> (
      host::local_application_dispatch_t{owner, record, std::move (parts)});
    ASSERT_TRUE (node->native_node ().transport ().mailbox ().try_enqueue (std::move (queued)));
    ASSERT_EQ (std::future_status::ready, entered.wait_for (1s));
    stop_thread = std::thread ([&] {
        stop_started.set_value ();
        view->host->stop ();
        stop_completed.set_value ();
    });
    ASSERT_EQ (std::future_status::ready, started.wait_for (1s));
    EXPECT_EQ (std::future_status::timeout, completed.wait_for (1s))
      << "host.stop returned before the ordinary handler physically completed";
    EXPECT_EQ (0, state.physical_terminal);
    EXPECT_EQ (0, state.destroyed);
    cleanup.run ();
    EXPECT_EQ (std::future_status::ready, completed.wait_for (0s));
    EXPECT_EQ (1, state.physical_terminal);
    EXPECT_EQ (1, state.destroyed);
    EXPECT_EQ (0, run_exit);
    EXPECT_NO_THROW (view->failures->rethrow_if_failed ());
}

struct payload_handler_t
{
    const zlink::message_t *payload = nullptr;
    int references = 0;

    void send (const int &, const route_message_context_t &) { references = payload->ref_count (); }

    task_t<int> request (const int &value, const route_message_context_t &)
    {
        references = payload->ref_count ();
        co_return value;
    }
};

struct send_payload_t
{
    static constexpr const char *packet_name = "p";
    int value = 7;
    std::shared_ptr<int> lifetime;
};
}

template <> struct zlink::json::dto_traits<send_payload_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("value", &send_payload_t::value)};
};

namespace
{


TEST (RoutePayloadOwnership, TypedSendRetainsPayloadWhenSerializerDestroysTheCall)
{
    std::optional<route_send_call_t> call;
    std::weak_ptr<int> lifetime;
    bool alive_during_serializer = false;
    serializer_registry_t serializers;
    serializers.add<send_payload_t> (
      [&] (const send_payload_t &payload) -> encoded_payload_t {
          EXPECT_EQ (7, payload.value);
          call.reset ();
          // The payload reference is no longer read after the call is destroyed.
          alive_during_serializer = !lifetime.expired ();
          throw framework_exception_t (framework_error_kind_t::protocol_error,
                                       "serializer rejected the payload");
      },
      [] (const encoded_payload_t &) { return send_payload_t{}; }, "x/y");
    auto builder = test::runtime_failure_builder ();
    auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    bool delivered = false;
    runtime.bind_mesh_node_transport (
      "m",
      [&] (const zlink::routing_id_t &,
           runtime::messaging::message_parts_t) -> task_t<result_t<void>> {
          delivered = true;
          co_return result_t<void>::success ();
      },
      {});
    auto client = builder.route_client (serializers);
    auto token = std::make_shared<int> (7);
    lifetime = token;
    call.emplace (client.send_to_node ("m", zlink::routing_id_t::from ("n"),
                                       send_payload_t{7, std::move (token)}));
    const auto submitted = call->async ();
    const auto &result = submitted.result ();
    EXPECT_FALSE (result);
    ASSERT_NE (nullptr, result.error ());
    EXPECT_EQ (framework_error_kind_t::protocol_error, result.error_kind ());
    EXPECT_STREQ ("serializer rejected the payload", result.error ()->what ());
    EXPECT_TRUE (alive_during_serializer);
    EXPECT_TRUE (lifetime.expired ());
    EXPECT_FALSE (delivered);
}

// Size of the one allocation that `make` performs: a task's shared state.
template <typename TMake> std::size_t single_allocation_size (TMake make)
{
    std::array<tests::allocation_event_t, 8> events{};
    tests::allocation_event_log_t log{events.data (), events.size ()};
    tests::allocation_event_log = &log;
    const auto count = tests::count_allocations (make);
    tests::allocation_event_log = nullptr;
    EXPECT_EQ (1u, count);
    return count == 1 ? events[0].size : 0;
}

struct route_send_fixture_t
{
    serializer_registry_t serializers;
    zlink_builder_t builder = test::runtime_failure_builder ();
    std::optional<route_client_t> client;

    template <typename TSend> explicit route_send_fixture_t (TSend send)
    {
        serializers.add<send_payload_t> (
          [] (const send_payload_t &) { return encoded_payload_t::from_string ("7"); },
          [] (const encoded_payload_t &) { return send_payload_t{}; }, "x/y");
        auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_serializers (serializers);
        runtime.bind_mesh_node_transport ("m", std::move (send), {});
        client.emplace (builder.route_client (serializers));
    }

    route_send_call_t send () { return client->send_to_node ("m", rid, send_payload_t{}); }

    zlink::routing_id_t rid = zlink::routing_id_t::from ("n");
};

// Submit/completion §4/§7 and hot path E4/E5: a route send owns one public task.
// The transport boundary is the only coroutine between the call and the transport task.
TEST (RoutePayloadOwnership, NodeSendCreatesOneTaskAboveTheTransport)
{
    const auto public_state =
      single_allocation_size ([] { task_t<void> ready (result_t<void>::success ()); });
    const auto transport_state = single_allocation_size ([] {
        task_t<result_t<void>> ready (
          result_t<result_t<void>>::success (result_t<void>::success ()));
    });
    ASSERT_NE (0u, public_state);
    ASSERT_NE (0u, transport_state);
    ASSERT_NE (public_state, transport_state);
    int delivered = 0;
    route_send_fixture_t fixture (
      [&] (const zlink::routing_id_t &, runtime::messaging::message_parts_t) {
          ++delivered;
          return task_t<result_t<void>> (
            result_t<result_t<void>>::success (result_t<void>::success ()));
      });
    for (int warmup = 0; warmup < 4; ++warmup) {
        auto call = fixture.send ();
        call.async ().result ().value ();
    }
    auto call = fixture.send ();
    std::array<tests::allocation_event_t, 256> events{};
    tests::allocation_event_log_t log{events.data (), events.size ()};
    tests::allocation_event_log = &log;
    const auto total = tests::count_allocations ([&] { call.async ().result ().value (); });
    tests::allocation_event_log = nullptr;
    ASSERT_FALSE (log.overflow);
    std::size_t public_states = 0;
    std::size_t transport_states = 0;
    std::ostringstream sizes;
    for (std::size_t index = 0; index < log.count; ++index) {
        sizes << events[index].size << ' ';
        public_states += events[index].size == public_state ? 1 : 0;
        transport_states += events[index].size == transport_state ? 1 : 0;
    }
    std::cout << "route-send allocations=" << total << " public-task-states=" << public_states
              << " result-task-states=" << transport_states << " sizes=" << sizes.str () << '\n';
    EXPECT_EQ (5, delivered);
    // One public task<void> and the transport's own ready task. A wrapper coroutine
    // between them adds a task state and a frame for every message.
    EXPECT_EQ (1u, public_states);
    EXPECT_EQ (1u, transport_states);
}

// Hot path E4: a deferred transport exception becomes internal_failure at the transport
// boundary, and a transport failure result keeps its Framework error.
TEST (RoutePayloadOwnership, NodeSendProjectsDeferredTransportFailures)
{
    task_completion_source_t<void> release;
    auto released = release.task ();
    route_send_fixture_t thrown (
      [&] (const zlink::routing_id_t &,
           runtime::messaging::message_parts_t) -> task_t<result_t<void>> {
          co_await std::move (released);
          throw std::runtime_error ("deferred route failure");
      });
    auto pending = thrown.send ().async ();
    release.complete (result_t<void>::success ());
    const auto &deferred = pending.result ();
    ASSERT_FALSE (deferred);
    ASSERT_NE (nullptr, deferred.error ());
    EXPECT_EQ (framework_error_kind_t::internal_failure, deferred.error_kind ());
    EXPECT_STREQ ("deferred route failure", deferred.error ()->what ());

    route_send_fixture_t rejected (
      [] (const zlink::routing_id_t &, runtime::messaging::message_parts_t) {
          return task_t<result_t<void>> (result_t<result_t<void>>::success (
            result_t<void>::failure (framework_error_kind_t::unavailable, "route lost")));
      });
    const auto failed = rejected.send ().async ();
    ASSERT_FALSE (failed.result ());
    EXPECT_EQ (framework_error_kind_t::unavailable, failed.result ().error_kind ());
    EXPECT_STREQ ("route lost", failed.result ().error ()->what ());

    auto once = rejected.send ();
    auto copy = once;
    (void) once.async ().result ();
    const auto duplicate = copy.async ();
    ASSERT_FALSE (duplicate.result ());
    EXPECT_EQ (framework_error_kind_t::protocol_error, duplicate.result ().error_kind ());
}

struct route_boundary_handler_t
{
    int calls = 0;
    void send (const int &, const route_message_context_t &) { ++calls; }
    int request (const int &value, const route_message_context_t &)
    {
        ++calls;
        return value;
    }
};

TEST (RouteHandlerBoundary, ResolverDoesNotAllocateKeys)
{
    detail::route_handler_registry_t handlers;
    const std::string mesh (2048, 'm');
    const std::string packet (4096, 'p');
    const std::string other_mesh (2048, 'x');
    const std::string other_packet (4096, 'x');
    handlers.on_send<route_boundary_handler_t, int> (mesh, packet, &route_boundary_handler_t::send);
    const detail::route_handler_registry_t::entry_t *hit = nullptr;
    const detail::route_handler_registry_t::entry_t *wrong_mesh = nullptr;
    const detail::route_handler_registry_t::entry_t *wrong_packet = nullptr;
    const detail::route_handler_registry_t::entry_t *wrong_kind = nullptr;
    const auto allocations = tests::count_allocations ([&] {
        hit = handlers.resolve (mesh, runtime::messaging::message_kind_t::command, packet);
        wrong_mesh =
          handlers.resolve (other_mesh, runtime::messaging::message_kind_t::command, packet);
        wrong_packet =
          handlers.resolve (mesh, runtime::messaging::message_kind_t::command, other_packet);
        wrong_kind = handlers.resolve (mesh, runtime::messaging::message_kind_t::request, packet);
    });
    EXPECT_EQ (0u, allocations);
    ASSERT_NE (nullptr, hit);
    EXPECT_EQ (mesh, hit->descriptor.router_channel_id);
    EXPECT_EQ (packet, hit->descriptor.packet_name);
    EXPECT_EQ (nullptr, wrong_mesh);
    EXPECT_EQ (nullptr, wrong_packet);
    EXPECT_EQ (nullptr, wrong_kind);
}

struct copy_counting_permit_t
{
    int *copies;
    int *calls;
    const route_boundary_handler_t *handler;
    std::array<std::byte, 1024> storage{};

    copy_counting_permit_t (int &copy_count,
                            int &call_count,
                            const route_boundary_handler_t &owner) :
        copies (&copy_count), calls (&call_count), handler (&owner)
    {
    }
    copy_counting_permit_t (const copy_counting_permit_t &other) :
        copies (other.copies), calls (other.calls), handler (other.handler)
    {
        ++*copies;
    }
    void operator() () const
    {
        ++*calls;
        EXPECT_EQ (1, *calls);
        EXPECT_EQ (0, handler->calls);
    }
};

TEST (RouteHandlerBoundary, DispatcherOwnsPermitCallbackForSuspension)
{
    for (const auto mode : {message_flow_log_mode_t::off, message_flow_log_mode_t::normal}) {
        for (const auto kind : {runtime::messaging::message_kind_t::command,
                                runtime::messaging::message_kind_t::request}) {
            SCOPED_TRACE (std::to_string (static_cast<int> (mode)) + ":"
                          + std::to_string (static_cast<int> (kind)));
            service_collection_t services;
            services.add_singleton<route_boundary_handler_t> ();
            auto provider = services.build_provider ();
            auto &owner = provider.get_required<route_boundary_handler_t> ();
            serializer_registry_t serializers;
            detail::route_handler_registry_t handlers;
            handlers.on_send<route_boundary_handler_t, int> ("mesh", "packet",
                                                             &route_boundary_handler_t::send);
            handlers.on_request<route_boundary_handler_t, int, int> (
              "mesh", "packet", &route_boundary_handler_t::request);
            detail::no_route_internal_packet_dispatcher_t internal;
            int copies = 0;
            int calls = 0;
            auto observations =
              std::make_shared<test::telemetry_observations_t<message_flow_event_t>> ();
            std::function<void ()> permit = copy_counting_permit_t (copies, calls, owner);
            dispatch_options_t options;
            options.message_flow (mode);
            detail::dispatch_options_access_t::set_observer_for_tests (
              options, [observations] (const message_flow_event_t &event) {
                  if (event.outcome == message_flow_outcome_t::dispatched)
                      observations->record (event);
              });
            detail::route_packet_dispatcher_t dispatcher (
              "mesh", provider, serializers, handlers, internal, options, nullptr,
              handler_dispatch_kind_t::node_direct_send,
              handler_dispatch_kind_t::node_direct_request, permit);
            runtime::messaging::envelope_header_t header;
            header.kind = kind;
            header.channel_name = "mesh";
            header.message_name = "packet";
            header.content_type = "application/json";
            header.correlation_id = "01890a5d-ac96-774b-bcce-b302099a8057";
            auto encoded = runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (
              header, zlink::message_t::from (std::string ("42")));
            detail::route_received_packet_t received{zlink::routing_id_t::from ("source"), 1,
                                                     std::move (encoded)};
            copies = 0;
            ASSERT_TRUE (dispatcher.dispatch (received, header).result ());
            EXPECT_EQ (1, copies);
            EXPECT_EQ (1, calls);
            EXPECT_EQ (1, owner.calls);
            test::acknowledge_delivery ();
            EXPECT_EQ (mode == message_flow_log_mode_t::off ? 0u : 1u,
                       observations->snapshot ().size ());
        }
    }
}

TEST (RouteHandlerBoundary, DispatcherResolvesHandlerOnce)
{
    service_collection_t services;
    services.add_singleton<route_boundary_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    detail::route_handler_registry_t handlers;
    const std::string packet (4096, 'p');
    handlers.on_send<route_boundary_handler_t, int> ("mesh", packet,
                                                     &route_boundary_handler_t::send);
    detail::no_route_internal_packet_dispatcher_t internal;
    detail::route_packet_dispatcher_t dispatcher ("mesh", provider, serializers, handlers,
                                                  internal);
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::command;
    header.channel_name = "mesh";
    header.message_name = packet;
    header.content_type = "application/json";
    auto encoded = runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (
      header, zlink::message_t::from (std::string ("42")));
    detail::route_received_packet_t received{zlink::routing_id_t::from ("source"), std::nullopt,
                                             std::move (encoded)};
    ASSERT_TRUE (dispatcher.dispatch (received, header).result ());
    // Only the header and context copy the packet name; lookup borrows both key strings.
    const auto copies = tests::count_allocations (
      [&] { EXPECT_TRUE (dispatcher.dispatch (received, header).result ()); }, packet.size () + 1);
    EXPECT_EQ (2u, copies);
    EXPECT_EQ (2, provider.get_required<route_boundary_handler_t> ().calls);
}

TEST (RouteHandlerBoundary, DecodeFailureDoesNotReleasePermitForHandlerStart)
{
    service_collection_t services;
    services.add_singleton<route_boundary_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    bool released = false;
    bool decoded = false;
    serializers.add<int> ([] (const int &) { return encoded_payload_t{}; },
                          [&] (const encoded_payload_t &) -> int {
                              EXPECT_FALSE (released);
                              decoded = true;
                              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                                           "decode failed");
                          },
                          "application/json");
    detail::route_handler_registry_t handlers;
    handlers.on_send<route_boundary_handler_t, int> ("mesh", "packet",
                                                     &route_boundary_handler_t::send);
    handler_registry_t filters;
    auto payload = zlink::message_t::from (std::string ("42"));
    route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
    context.content_type = "application/json";
    auto result =
      detail::route_handler_invoker_t{}
        .invoke (*handlers.resolve ("mesh", runtime::messaging::message_kind_t::command, "packet"),
                 filters, handler_dispatch_kind_t::node_direct_send, provider, serializers, payload,
                 context, [&] { released = true; })
        .result ();
    EXPECT_FALSE (result);
    EXPECT_TRUE (decoded);
    EXPECT_FALSE (released);
    EXPECT_EQ (0, provider.get_required<route_boundary_handler_t> ().calls);
}

TEST (RouteHandlerBoundary, UnknownWireContentTypeIsProtocolError)
{
    service_collection_t services;
    services.add_singleton<route_boundary_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    detail::route_handler_registry_t handlers;
    handlers.on_send<route_boundary_handler_t, int> ("mesh", "packet",
                                                     &route_boundary_handler_t::send);
    handler_registry_t filters;
    auto payload = zlink::message_t::from (std::string ("42"));
    route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
    context.content_type = "application/unregistered";
    bool released = false;
    auto result =
      detail::route_handler_invoker_t{}
        .invoke (*handlers.resolve ("mesh", runtime::messaging::message_kind_t::command, "packet"),
                 filters, handler_dispatch_kind_t::node_direct_send, provider, serializers, payload,
                 context, [&] { released = true; })
        .result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::protocol_error, result.error_kind ());
    EXPECT_FALSE (released);
    EXPECT_EQ (0, provider.get_required<route_boundary_handler_t> ().calls);
}

struct ordered_route_handler_t
{
    std::vector<int> *order;
    void send (const int &value, const route_message_context_t &)
    {
        EXPECT_EQ (42, value);
        order->push_back (4);
    }
    task_t<void> send_async (const int &value, const route_message_context_t &context)
    {
        send (value, context);
        co_return;
    }
    int request (const int &value, const route_message_context_t &context)
    {
        send (value, context);
        return value;
    }
    task_t<int> request_async (const int &value, const route_message_context_t &context)
    {
        co_return request (value, context);
    }
};

TEST (RouteHandlerBoundary, TypedInvokersReleaseAfterResolveAndDecode)
{
    for (int form = 0; form != 4; ++form) {
        for (int failure = 0; failure != 3; ++failure) {
            SCOPED_TRACE (std::to_string (form) + ":" + std::to_string (failure));
            std::vector<int> order;
            service_collection_t services;
            services.add_factory<ordered_route_handler_t> ([&] (service_provider_t &) {
                order.push_back (1);
                return std::make_unique<ordered_route_handler_t> (&order);
            });
            auto provider = services.build_provider ();
            serializer_registry_t serializers;
            serializers.add<int> (
              [] (const int &value) {
                  return encoded_payload_t::from_string (std::to_string (value));
              },
              [&] (const encoded_payload_t &) {
                  order.push_back (2);
                  if (failure == 1)
                      throw framework_exception_t (framework_error_kind_t::protocol_error,
                                                   "decode failed");
                  return 42;
              },
              "application/x-route-test");
            detail::route_handler_registry_t handlers;
            handlers.on_send<ordered_route_handler_t, int> ("mesh", "sync-send",
                                                            &ordered_route_handler_t::send);
            handlers.on_send<ordered_route_handler_t, int> ("mesh", "async-send",
                                                            &ordered_route_handler_t::send_async);
            handlers.on_request<ordered_route_handler_t, int, int> (
              "mesh", "sync-request", &ordered_route_handler_t::request);
            handlers.on_request<ordered_route_handler_t, int, int> (
              "mesh", "async-request", &ordered_route_handler_t::request_async);
            const auto kind = form < 2 ? runtime::messaging::message_kind_t::command
                                       : runtime::messaging::message_kind_t::request;
            const std::array names{"sync-send", "async-send", "sync-request", "async-request"};
            const auto *entry = handlers.resolve ("mesh", kind, names[form]);
            ASSERT_NE (nullptr, entry);
            auto message = zlink::message_t::from (std::string ("wire payload"));
            route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
            context.content_type =
              failure == 2 ? "application/unregistered" : "application/x-route-test";
            handler_registry_t filters;
            detail::route_handler_invoker_t invoker;
            auto before = [&] { order.push_back (3); };
            if (form < 2) {
                const auto result =
                  invoker
                    .invoke (*entry, filters, handler_dispatch_kind_t::node_direct_send, provider,
                             serializers, message, context, before)
                    .result ();
                EXPECT_EQ (failure == 0, static_cast<bool> (result));
                if (failure != 0)
                    EXPECT_EQ (framework_error_kind_t::protocol_error, result.error_kind ());
            } else {
                const auto result =
                  invoker
                    .invoke (*entry, filters, handler_dispatch_kind_t::node_direct_request,
                             provider, serializers, message, context, before)
                    .result ();
                EXPECT_EQ (failure == 0, static_cast<bool> (result));
                if (failure != 0)
                    EXPECT_EQ (framework_error_kind_t::protocol_error, result.error_kind ());
            }
            const auto expected = failure == 0   ? std::vector<int>{1, 2, 3, 4}
                                  : failure == 1 ? std::vector<int>{1, 2}
                                                 : std::vector<int>{1};
            EXPECT_EQ (expected, order);
        }
    }
}

TEST (RoutePayloadOwnership, SendOwnsPayloadForTheAwaitedHandler)
{
    service_collection_t services;
    services.add_singleton<payload_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    detail::route_handler_registry_t handlers;
    handlers.on_send<payload_handler_t, int> ("mesh", "payload", &payload_handler_t::send);
    handler_registry_t filters;
    auto payload = zlink::message_t::from (std::string (128, ' ') + "42");
    auto &handler = provider.get_required<payload_handler_t> ();
    handler.payload = &payload;
    const auto references = payload.ref_count ();
    detail::route_handler_invoker_t invoker;
    route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
    auto invoked =
      invoker
        .invoke (*handlers.resolve ("mesh", runtime::messaging::message_kind_t::command, "payload"),
                 filters, handler_dispatch_kind_t::node_direct_send, provider, serializers, payload,
                 context)
        .result ();
    ASSERT_TRUE (invoked);
    EXPECT_EQ (handler.references, references + 1);
    EXPECT_EQ (payload.ref_count (), references);
}

TEST (RoutePayloadOwnership, RequestOwnsPayloadForTheAwaitedHandler)
{
    service_collection_t services;
    services.add_singleton<payload_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    detail::route_handler_registry_t handlers;
    handlers.on_request<payload_handler_t, int, int> ("mesh", "payload",
                                                      &payload_handler_t::request);
    handler_registry_t filters;
    auto payload = zlink::message_t::from (std::string (128, ' ') + "42");
    auto &handler = provider.get_required<payload_handler_t> ();
    handler.payload = &payload;
    const auto references = payload.ref_count ();
    detail::route_handler_invoker_t invoker;
    route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
    auto invoked =
      invoker
        .invoke (*handlers.resolve ("mesh", runtime::messaging::message_kind_t::request, "payload"),
                 filters, handler_dispatch_kind_t::node_direct_request, provider, serializers,
                 payload, context)
        .result ();
    ASSERT_TRUE (invoked);
    EXPECT_EQ (handler.references, references + 1);
    EXPECT_EQ (payload.ref_count (), references);
}

TEST (RoutePayloadOwnership, ApplicationDispatcherConsumesTheDecodedHeader)
{
    service_collection_t services;
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    services.add_singleton<payload_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    detail::route_handler_registry_t handlers;
    handlers.on_send<payload_handler_t, int> ("mesh", "payload", &payload_handler_t::send);
    handler_registry_t filters;
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::command;
    header.channel_name = "mesh";
    header.message_name = "payload";
    auto encoded = runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (
      header, zlink::message_t::from (std::string (128, ' ') + "42"));
    auto parts = std::move (encoded).take_items ();
    auto &handler = provider.get_required<payload_handler_t> ();
    handler.payload = &parts[1];
    auto state = std::make_shared<detail::spot_node_builder_state_t> ("node");
    detail::spot_node_runtime_t spots (state);
    spots.set_route_client (route_client_t{});
    runtime::host::ready_record_t owner;
    owner.owner_kind = runtime::host::owner_kind_t::node;
    runtime::host::receive_record_t record;
    record.kind = runtime::host::record_kind_t::node_send;
    record.channel_name = "mesh";
    auto decoded = runtime::decode_mesh_record_header (record, parts, false);
    EXPECT_FALSE (
      spots.dispatch_mesh_record (owner, record, parts, decoded.value (), provider, serializers));
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (decoded.value ());
    // A consumer of the parsed header must never consult its wire frame again.
    parts[0] = zlink::message_t::from (std::string ("invalid header"));
    detail::mesh_record_dispatcher_t dispatcher (provider, serializers, handlers, filters);
    ASSERT_TRUE (
      dispatcher.dispatch (record, std::move (parts), std::move (*decoded.value ())).result ());
    EXPECT_GT (handler.references, 0);
}

TEST (RoutePayloadOwnership, MeshIngressProvidesHeaderWithoutARouteClient)
{
    service_collection_t services;
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    auto state = std::make_shared<detail::spot_node_builder_state_t> ("node");
    detail::spot_node_runtime_t spots (state);
    runtime::host::ready_record_t owner;
    owner.owner_kind = runtime::host::owner_kind_t::node;
    runtime::host::receive_record_t record;
    record.kind = runtime::host::record_kind_t::node_send;
    runtime::messaging::envelope_header_t header;
    header.channel_name = "mesh";
    header.message_name = "payload";
    auto parts = runtime::messaging::envelope_codec_t{}
                   .encode_raw_body_parts (header, zlink::message_t::from ("42"))
                   .take_items ();
    auto decoded = runtime::decode_mesh_record_header (record, parts, false);
    EXPECT_FALSE (
      spots.dispatch_mesh_record (owner, record, parts, decoded.value (), provider, serializers));
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (decoded.value ());
    EXPECT_EQ (decoded.value ()->message_name, "payload");
}

TEST (RoutePayloadOwnership, ActorSendFallbackProvidesHeader)
{
    service_collection_t services;
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    auto state = std::make_shared<detail::spot_node_builder_state_t> ("node");
    detail::spot_node_runtime_t spots (state);
    runtime::host::ready_record_t owner;
    owner.owner_kind = runtime::host::owner_kind_t::node;
    runtime::host::receive_record_t record;
    record.kind = runtime::host::record_kind_t::actor_send;
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::command;
    header.channel_name = "actor";
    header.message_name = "payload";
    auto parts = detail::encode_actor_packet (header, zlink::message_t::from ("42")).take_items ();
    auto decoded = runtime::decode_mesh_record_header (record, parts, false);
    EXPECT_FALSE (
      spots.dispatch_mesh_record (owner, record, parts, decoded.value (), provider, serializers));
    EXPECT_NO_THROW ((void) decoded.value ());
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (decoded.value ());
    EXPECT_EQ (decoded.value ()->message_name, "payload");
}

TEST (RoutePayloadOwnership, ActorSendWithoutTerminalPreservesIngressHeaderAndFrames)
{
    service_collection_t services;
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    auto state = std::make_shared<detail::spot_node_builder_state_t> ("node");
    state->actor_types_by_id["actor"] = "player";
    detail::spot_node_runtime_t spots (state);
    runtime::host::ready_record_t owner;
    owner.owner_kind = runtime::host::owner_kind_t::actor;
    owner.actor =
      detail::actor_ref_access_t::make (node_rid_t::from_string ("node"), "player", "actor", 1);
    runtime::host::receive_record_t record;
    record.kind = runtime::host::record_kind_t::actor_send;
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::command;
    header.channel_name = "actor";
    header.message_name = "payload";
    auto parts = detail::encode_actor_packet (header, zlink::message_t::from ("42")).take_items ();
    auto decoded = runtime::decode_mesh_record_header (record, parts, false);
    parts[0] = zlink::message_t::from (std::string ("invalid header"));
    EXPECT_FALSE (
      spots.dispatch_mesh_record (owner, record, parts, decoded.value (), provider, serializers));
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (decoded.value ());
    EXPECT_EQ (decoded.value ()->message_name, "payload");
    ASSERT_EQ (parts.size (), 2u);
    EXPECT_EQ (parts[0].to_string (), "invalid header");
    EXPECT_EQ (parts[1].to_string (), "42");
}

TEST (RoutePayloadOwnership, MeshIngressSkipsRecordsWithoutEnvelopeHeaders)
{
    runtime::host::receive_record_t record;
    auto parts =
      std::vector<zlink::message_t>{zlink::message_t::from (std::string ("invalid header"))};
    for (auto kind : {runtime::host::record_kind_t::spot_multicast,
                      runtime::host::record_kind_t::spot_control}) {
        record.kind = kind;
        auto decoded = runtime::decode_mesh_record_header (record, parts, true);
        ASSERT_TRUE (decoded);
        EXPECT_FALSE (decoded.value ());
        ASSERT_EQ (parts.size (), 1u);
        EXPECT_EQ (parts[0].to_string (), "invalid header");
    }
}

TEST (RoutePayloadOwnership, MeshIngressRejectsMalformedRouteAndActorHeaders)
{
    runtime::host::receive_record_t record;
    for (auto kind :
         {runtime::host::record_kind_t::node_send, runtime::host::record_kind_t::actor_send,
          runtime::host::record_kind_t::actor_request}) {
        record.kind = kind;
        auto parts =
          std::vector<zlink::message_t>{zlink::message_t::from (std::string ("invalid header"))};
        auto decoded = runtime::decode_mesh_record_header (record, parts, true);
        EXPECT_FALSE (decoded);
        EXPECT_EQ (decoded.error_kind (), framework_error_kind_t::protocol_error);
        ASSERT_EQ (parts.size (), 1u);
        EXPECT_EQ (parts[0].to_string (), "invalid header");
    }
}

TEST (RoutePayloadOwnership, MeshIngressRepliesToInvalidRequestHeadersForEveryOwner)
{
    namespace host = runtime::host;
    namespace msg = runtime::messaging;
    auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "header-rejection",
                              .node_routing_id = zlink::routing_id_t::from ("reply").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    msg::envelope_codec_t codec;
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::normal);
    auto observations =
      std::make_shared<test::telemetry_observations_t<message_dispatch_error_event_t>> ();
    std::vector<message_dispatch_error_event_t> errors;
    detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      options, [observations] (const message_dispatch_error_event_t &event) {
          observations->record (event);
      });
    for (auto kind : {host::record_kind_t::spot_request, host::record_kind_t::node_request,
                      host::record_kind_t::channel_request}) {
        SCOPED_TRACE (static_cast<int> (kind));
        host::receive_record_t record;
        record.kind = kind;
        record.channel_name = "mesh";
        int replies = 0;
        std::vector<zlink::message_t> reply_parts;
        record.reply_token.host = reply_host;
        record.reply_token.local_reply = [&] (const std::vector<zlink::message_t> &parts) {
            ++replies;
            reply_parts = parts;
            return true;
        };
        msg::envelope_header_t header;
        header.channel_name = "mesh";
        header.message_name = "request";
        header.correlation_id = "request-correlation";
        header.flow_id = "01890a5d-ac96-774b-bcce-b302099a8057";
        header.flow_origin = flow_origin_t::application;
        auto wire = codec.encode_header (header).to_string ();
        auto invalid_flow = wire;
        invalid_flow.replace (invalid_flow.find (*header.flow_id), 4, "ZZZZ");
        auto invalid_flow_type = wire;
        const auto flow_at = invalid_flow_type.find (*header.flow_id);
        invalid_flow_type.replace (flow_at - 1, header.flow_id->size () + 2, "7");
        auto invalid_metadata = wire;
        const std::string empty_metadata = "\"metadata\":{}";
        invalid_metadata.replace (invalid_metadata.find (empty_metadata), empty_metadata.size (),
                                  "\"metadata\":[]");
        auto invalid_content_type = wire;
        const auto content_at = invalid_content_type.find (header.content_type);
        invalid_content_type.replace (content_at - 1, header.content_type.size () + 2, "7");
        auto invalid_deadline = wire;
        const std::string deadline = "\"deadline\":null";
        invalid_deadline.replace (invalid_deadline.find (deadline), deadline.size (),
                                  "\"deadline\":7");
        for (const auto &invalid : {invalid_flow, invalid_flow_type, invalid_metadata,
                                    invalid_content_type, invalid_deadline}) {
            SCOPED_TRACE (invalid);
            const auto previous_errors = errors.size ();
            const auto previous_replies = replies;
            reply_parts.clear ();
            auto parts = std::vector<zlink::message_t>{zlink::message_t::from (invalid),
                                                       zlink::message_t::from ("42")};
            auto rejected = runtime::decode_mesh_record_header (record, parts, true, options);
            EXPECT_FALSE (rejected);
            EXPECT_EQ (rejected.error_kind (), framework_error_kind_t::protocol_error);
            EXPECT_EQ (replies, previous_replies + 1);
            test::acknowledge_delivery ();
            errors = observations->snapshot ();
            ASSERT_EQ (errors.size (), previous_errors + 1);
            EXPECT_EQ (errors.back ().action, dispatch_error_action_t::reply_error);
            EXPECT_EQ (errors.back ().reason, dispatch_error_reason_t::invalid_frame);
            EXPECT_EQ (errors.back ().correlation_id, header.correlation_id);
            EXPECT_EQ (errors.back ().flow_id,
                       invalid == invalid_metadata ? header.flow_id : std::nullopt);
            ASSERT_EQ (reply_parts.size (), 2u);
            auto reply = codec.decode_header (msg::message_parts_t (reply_parts));
            ASSERT_TRUE (reply);
            EXPECT_EQ (reply.value ().kind, msg::message_kind_t::error);
            EXPECT_EQ (reply.value ().error_code, "protocol_error");
            EXPECT_EQ (reply.value ().correlation_id, header.correlation_id);
            EXPECT_EQ (reply.value ().channel_name, header.channel_name);
            EXPECT_EQ (reply.value ().message_name, header.message_name);
        }
        // Without readable correlation there is no request to attach a reply to.
        const auto previous_replies = replies;
        const auto previous_errors = errors.size ();
        auto unreadable = std::vector<zlink::message_t>{zlink::message_t::from ("invalid header")};
        EXPECT_FALSE (runtime::decode_mesh_record_header (record, unreadable, true, options));
        EXPECT_EQ (replies, previous_replies);
        test::acknowledge_delivery ();
        errors = observations->snapshot ();
        ASSERT_EQ (errors.size (), previous_errors + 1);
        EXPECT_EQ (errors.back ().action, dispatch_error_action_t::drop);
        EXPECT_FALSE (errors.back ().correlation_id);
    }
}

TEST (RoutePayloadOwnership, MeshIngressDropsInvalidSendsAndReportsOnceForEveryOwner)
{
    namespace host = runtime::host;
    namespace msg = runtime::messaging;
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::normal);
    auto observations =
      std::make_shared<test::telemetry_observations_t<message_dispatch_error_event_t>> ();
    std::vector<message_dispatch_error_event_t> errors;
    detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      options, [observations] (const message_dispatch_error_event_t &event) {
          observations->record (event);
      });
    for (auto kind : {host::record_kind_t::actor_send, host::record_kind_t::spot_send,
                      host::record_kind_t::node_send, host::record_kind_t::channel_send}) {
        SCOPED_TRACE (static_cast<int> (kind));
        host::receive_record_t record;
        record.kind = kind;
        record.channel_name = "mesh";
        int replies = 0;
        record.reply_token.local_reply = [&] (const std::vector<zlink::message_t> &) {
            ++replies;
            return true;
        };
        auto parts = std::vector<zlink::message_t>{zlink::message_t::from ("invalid header")};
        const auto previous_errors = errors.size ();
        auto decoded = runtime::decode_mesh_record_header (record, parts, true, options);
        EXPECT_FALSE (decoded);
        EXPECT_EQ (decoded.error_kind (), framework_error_kind_t::protocol_error);
        EXPECT_EQ (replies, 0);
        test::acknowledge_delivery ();
        errors = observations->snapshot ();
        ASSERT_EQ (errors.size (), previous_errors + 1);
        EXPECT_EQ (errors.back ().reason, dispatch_error_reason_t::invalid_frame);
        EXPECT_EQ (errors.back ().action, dispatch_error_action_t::drop);
        EXPECT_EQ (errors.back ().message_kind, dispatch_message_kind_t::send);
        EXPECT_EQ (errors.back ().surface,
                   kind == host::record_kind_t::actor_send  ? dispatch_error_surface_t::spot_actor
                   : kind == host::record_kind_t::spot_send ? dispatch_error_surface_t::spot_route
                   : kind == host::record_kind_t::node_send
                     ? dispatch_error_surface_t::node
                     : dispatch_error_surface_t::route_mesh_channel);
        EXPECT_FALSE (errors.back ().flow_id);
        EXPECT_FALSE (errors.back ().correlation_id);
        ASSERT_EQ (parts.size (), 1u);
        EXPECT_EQ (parts[0].to_string (), "invalid header");
    }
}

class client_node_store_t : public runtime::in_memory_location_repository_t
{
  public:
    int lists = 0;
    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh_name, location_page_request_t = {}) override
    {
        ++lists;
        mesh_node_descriptor_t target;
        target.mesh_name = std::move (mesh_name);
        target.rid = zlink::routing_id_t::from ("session-node");
        target.object_role = object_role_t::client;
        location_page_t<mesh_node_descriptor_t> page;
        page.items.push_back (std::move (target));
        co_return page;
    }
};

TEST (RoutePayloadOwnership, MalformedReplyRouteMatchesSharedFourLanguageContract)
{
    namespace host = runtime::host;
    namespace msg = runtime::messaging;
    auto root = std::filesystem::path (__FILE__).parent_path ();
    for (int depth = 0; depth < 4; ++depth)
        root = root.parent_path ();
    std::ifstream fixture (root / "test/fixtures/malformed-envelope-reply-route.tsv");
    ASSERT_TRUE (fixture.is_open ());
    auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "header-rejection",
                              .node_routing_id = zlink::routing_id_t::from ("reply").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    std::string row;
    int count = 0;
    while (std::getline (fixture, row)) {
        if (!row.empty () && row.back () == '\r')
            row.pop_back ();
        if (row.empty () || row.front () == '#')
            continue;
        std::istringstream input (row);
        std::string id, request, json, action;
        std::getline (input, id, '\t');
        std::getline (input, request, '\t');
        std::getline (input, json, '\t');
        std::getline (input, action, '\t');
        SCOPED_TRACE (id);
        for (auto kind : request == "true" ? std::vector{host::record_kind_t::node_request,
                                                         host::record_kind_t::channel_request,
                                                         host::record_kind_t::spot_request}
                                           : std::vector{host::record_kind_t::node_send,
                                                         host::record_kind_t::channel_send,
                                                         host::record_kind_t::spot_send}) {
            SCOPED_TRACE (static_cast<int> (kind));
            host::receive_record_t record;
            record.kind = kind;
            record.channel_name = "play";
            int replies = 0;
            std::vector<zlink::message_t> reply_parts;
            if (request == "true") {
                record.reply_token.host = reply_host;
                record.reply_token.local_reply = [&] (const std::vector<zlink::message_t> &parts) {
                    ++replies;
                    reply_parts = parts;
                    return true;
                };
            }
            dispatch_options_t options;
            options.message_flow (message_flow_log_mode_t::normal);
            auto observations =
              std::make_shared<test::telemetry_observations_t<message_dispatch_error_event_t>> ();
            std::vector<message_dispatch_error_event_t> errors;
            detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
              options, [observations] (const message_dispatch_error_event_t &event) {
                  observations->record (event);
              });
            auto parts = std::vector<zlink::message_t>{zlink::message_t::from (json),
                                                       zlink::message_t::from ("request")};
            auto result = runtime::decode_mesh_record_header (record, parts, false, options);
            EXPECT_FALSE (result);
            EXPECT_EQ (replies, action == "reply_error" ? 1 : 0);
            test::acknowledge_delivery ();
            errors = observations->snapshot ();
            ASSERT_EQ (errors.size (), 1u);
            EXPECT_EQ (errors[0].reason, dispatch_error_reason_t::invalid_frame);
            EXPECT_EQ (errors[0].action, action == "reply_error"
                                           ? dispatch_error_action_t::reply_error
                                           : dispatch_error_action_t::drop);
            if (!reply_parts.empty ()) {
                auto reply =
                  msg::envelope_codec_t{}.decode_header (msg::message_parts_t (reply_parts), false);
                ASSERT_TRUE (reply);
                EXPECT_EQ (reply.value ().kind, msg::message_kind_t::error);
                EXPECT_EQ (reply.value ().error_code, "protocol_error");
                EXPECT_EQ (reply.value ().correlation_id,
                           nlohmann::json::parse (json).at ("correlationId").get<std::string> ());
            }
        }
        ++count;
    }
    EXPECT_EQ (count, 22);
}

namespace
{
std::shared_ptr<detail::mesh_node_builder_state_t> make_wire_test_node (const char *rid,
                                                                        object_role_t role)
{
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("mesh");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->routing_id = zlink::routing_id_t::from (rid);
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->listen_port.reset ();
    state->object_role = role;
    state->channels.emplace ("work", detail::mesh_channel_registration_t{100, {}, true, true});
    return state;
}
}

TEST (RoutePayloadOwnership, BoundSessionInternalSendSkipsUserTargetClassification)
{
    auto source_state = make_wire_test_node ("actor-node", object_role_t::server);
    auto target_state = make_wire_test_node ("session-node", object_role_t::client);
    detail::mesh_node_runtime_t source (source_state), target (target_state);
    auto store = std::make_shared<client_node_store_t> ();
    source.configure_user_spot_operations (store, {});
    source.start ();
    target.start ();
    source.connect_peer (*target_state->routing_id, target.status ().local_endpoint ());
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    auto pump = [] (detail::mesh_node_runtime_t &node) {
        node.dispatch_ready ([] (const auto &, const auto &, auto) {}).result ().value ();
    };
    while ((source.admitted_peer_count () == 0 || target.admitted_peer_count () == 0)
           && std::chrono::steady_clock::now () < deadline) {
        pump (source);
        pump (target);
        std::this_thread::yield ();
    }
    ASSERT_EQ (source.admitted_peer_count (), 1u);
    ASSERT_EQ (target.admitted_peer_count (), 1u);
    serializer_registry_t serializers;
    auto builder = test::runtime_failure_builder ();
    auto client = builder.route_client (serializers);
    detail::channel_runtime_t::from (builder.message_bus ())
      .bind_mesh_node_transport (
        "mesh",
        [&source] (const auto &rid,
                   runtime::messaging::message_parts_t parts) -> task_t<result_t<void>> {
            const auto sent = co_await source.send_to_node (rid, std::move (parts).take_items ());
            if (sent == zlink::submit_result_t::ok)
                co_return result_t<void>::success ();
            co_return result_t<void>::failure (
              runtime::messaging::map_submit_result_error_kind (sent), "node send failed");
        },
        {});
    detail::actor_gateway_runtime_t gateway;
    const auto actor = detail::actor_ref_access_t::make (node_rid_t::from_string ("actor-node"),
                                                         "player", "actor", 1);
    const auto user_send =
      client.send_to_node ("mesh", *target_state->routing_id, 42).async ().result ();
    ASSERT_FALSE (user_send);
    EXPECT_EQ (user_send.error_kind (), framework_error_kind_t::not_found);
    ASSERT_TRUE (gateway.bind_session_route (
      actor, detail::spot_node_runtime_t (source_state->spot_state).native_node (), serializers,
      "mesh", *target_state->routing_id));
    store->lists = 0;
    const auto sent = gateway.dispatch_bound_session_send (actor, "reply", stream_codec_t::json,
                                                           zlink::message_t::from ("42"));
    EXPECT_TRUE (sent) << (sent.error () ? sent.error ()->what () : "");
    EXPECT_EQ (store->lists, 0);
    bool delivered = false;
    const auto receive_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (sent && !delivered && std::chrono::steady_clock::now () < receive_deadline) {
        target
          .dispatch_ready ([&] (const auto &, const auto &, auto parts) {
              auto decoded = runtime::messaging::envelope_codec_t{}.decode_header (
                runtime::messaging::message_parts_t (std::move (parts)), false);
              delivered = decoded
                          && decoded.value ().message_name
                               == detail::actor_bound_session_route_request_t::packet_name;
          })
          .result ()
          .value ();
        std::this_thread::yield ();
    }
    EXPECT_TRUE (delivered);
    source.stop ();
    target.stop ();
}

TEST (RoutePayloadOwnership, UnconnectedObjectClientNodeDirectTargetIsNotFound)
{
    // 02-channel-messaging section 8: an Object Client RID is not a Node
    // direct target. A target the topology does not know is classified from
    // the Location Store, so the result does not depend on a connection.
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("mesh");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->routing_id = zlink::routing_id_t::from ("actor-node");
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->listen_port.reset ();
    state->object_role = object_role_t::server;
    detail::mesh_node_runtime_t source (state);
    auto store = std::make_shared<client_node_store_t> ();
    source.configure_user_spot_operations (store, {});
    source.start ();
    const auto client_rid = zlink::routing_id_t::from ("session-node");
    const std::vector<zlink::message_t> parts{zlink::message_t::from (std::string ("payload"))};
    EXPECT_EQ (std::move (source.send_to_node (client_rid, parts)).result ().value (),
               zlink::submit_result_t::not_found);
    runtime::host::pending_operation_t operation;
    EXPECT_EQ (std::move (source.request_to_node (client_rid, parts, operation,
                                                  std::chrono::milliseconds (500)))
                 .result ()
                 .value (),
               zlink::submit_result_t::not_found);
    EXPECT_EQ (store->lists, 2);
    source.stop ();
}

TEST (RoutePayloadOwnership, ServiceWirePartsRetainTheTransferredFrame)
{
    namespace protocol = runtime::protocol;
    std::vector<zlink::message_t> source;
    source.push_back (zlink::message_t::from (std::string ("header")));
    source.push_back (zlink::message_t::from (std::string (1024, 'x')));
    auto frame = protocol::encode_application_payload (
      protocol::application_payload_t::from_parts (std::move (source)));
    const auto start = reinterpret_cast<std::uintptr_t> (frame.data ());
    const auto end = start + frame.size ();
    detail::backend::raw_message_t input;
    input.push_back (std::move (frame));
    auto native = detail::backend::materialize_binding_parts (std::move (input));
    auto parts = protocol::decode_application_parts (std::move (native[0]), false);
    ASSERT_EQ (parts.size (), 2u);
    EXPECT_EQ (parts[0].to_string (), "header");
    EXPECT_EQ (parts[1].to_string (), std::string (1024, 'x'));
    for (const auto &part : parts) {
        const auto address = reinterpret_cast<std::uintptr_t> (part.data ());
        EXPECT_GE (address, start);
        EXPECT_LE (address + part.size (), end);
    }
}

TEST (RoutePayloadOwnership, ServiceWireRejectsTruncatedTransferredFrame)
{
    namespace protocol = runtime::protocol;
    auto frame = protocol::encode_application_payload (protocol::application_payload_t::from_parts (
      std::vector<zlink::message_t>{zlink::message_t::from (std::string ("payload"))}));
    frame.pop_back ();
    detail::backend::raw_message_t input;
    input.push_back (std::move (frame));
    auto native = detail::backend::materialize_binding_parts (std::move (input));
    EXPECT_THROW (protocol::decode_application_parts (std::move (native[0]), false),
                  protocol::service_wire_error_t);
}
}

TEST (RoutePayloadOwnership, ActorInvalidHeaderRepliesWithStreamErrorUsingOuterCorrelation)
{
    namespace host = runtime::host;
    auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "actor-header-rejection",
                              .node_routing_id = zlink::routing_id_t::from ("reply").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    host::receive_record_t record{.kind = host::record_kind_t::actor_request};
    record.reply_token.host = reply_host;
    std::vector<zlink::message_t> reply_parts;
    record.reply_token.local_reply = [&] (const std::vector<zlink::message_t> &parts) {
        reply_parts = parts;
        return true;
    };
    for (const auto &bad : {std::string ("BeginUserSpotJoinReq"), std::string ("{}")}) {
        auto parts = std::vector<zlink::message_t>{zlink::message_t::from (bad),
                                                   zlink::message_t::from ("{}")};
        auto decoded = runtime::decode_mesh_record_header (record, parts, false);
        ASSERT_FALSE (decoded);
        EXPECT_EQ (framework_error_kind_t::protocol_error, decoded.error_kind ());
        ASSERT_EQ (1u, reply_parts.size ());
        auto error = detail::decode_actor_reply (runtime::messaging::message_parts_t (reply_parts));
        ASSERT_FALSE (error);
        EXPECT_EQ (framework_error_kind_t::protocol_error, error.error_kind ());
    }
}

TEST (RoutePayloadOwnership, MissingActorHandlerRepliesWithStreamError)
{
    namespace host = runtime::host;
    auto state = test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("node");
    state->actor_types_by_id["actor"] = "player";
    state->actor_instances["player:actor"] = std::make_shared<int> (7);
    state->actor_generations["player:actor"] = 1;
    state->actor_spot_ids["player:actor"] = spot_id_t ("spot");
    detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    state->actor_factories.emplace ("player", std::move (factory));
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = state;
    context->spot_id = spot_id_t ("spot");
    context->spot_name = "spot";
    context->spot_instance = std::make_shared<int> (1);
    context->serial_executor = state->worker_executor;
    context->channel_runtime = test::runtime_failure_fixture<detail::channel_runtime_state_t> ();
    state->spot_contexts_by_id.emplace (context->spot_id,
                                        detail::spot_context_access_t::create (context));
    serializer_registry_t serializers;
    context->channel_runtime->serializers = &serializers;
    service_collection_t services;
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "actor-handler-rejection",
                              .node_routing_id = zlink::routing_id_t::from ("reply").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    std::promise<std::vector<zlink::message_t>> reply;
    auto received = reply.get_future ();
    host::receive_record_t record{.kind = host::record_kind_t::actor_request};
    record.reply_token.host = reply_host;
    record.reply_token.local_reply = [&] (const std::vector<zlink::message_t> &parts) {
        reply.set_value (parts);
        return true;
    };
    host::ready_record_t owner{.owner_kind = host::owner_kind_t::actor};
    owner.actor =
      detail::actor_ref_access_t::make (node_rid_t::from_string ("node"), "player", "actor", 1);
    runtime::messaging::envelope_header_t request;
    request.message_name = "MissingHandler";
    auto parts = detail::encode_actor_packet (request, zlink::message_t::from ("{}")).take_items ();
    auto decoded = runtime::decode_mesh_record_header (record, parts, false);
    ASSERT_TRUE (decoded);
    EXPECT_TRUE (detail::spot_node_runtime_t (state).dispatch_mesh_record (
      owner, record, parts, decoded.value (), provider, serializers));
    ASSERT_EQ (std::future_status::ready, received.wait_for (std::chrono::seconds (2)));
    auto frames = received.get ();
    ASSERT_EQ (1u, frames.size ());
    auto error =
      detail::decode_actor_reply (runtime::messaging::message_parts_t (std::move (frames)));
    ASSERT_FALSE (error);
    EXPECT_EQ (framework_error_kind_t::not_found, error.error_kind ());
    EXPECT_NE (std::string::npos,
               std::string (error.error ()->what ()).find ("spot handler is not registered"));
}

class ActorSendRelayAdmission : public ::testing::TestWithParam<std::tuple<bool, bool>>
{
};

TEST_P (ActorSendRelayAdmission, CompletesWithoutRemoteReply)
{
    const auto [await_remote_admission, ordered_relay] = GetParam ();
    auto source_state = make_wire_test_node ("wire-source", object_role_t::server);
    auto target_state = make_wire_test_node ("wire-target", object_role_t::server);
    detail::mesh_node_runtime_t source (source_state), target (target_state);
    source.start ();
    target.start ();
    source.connect_peer (*target_state->routing_id, target.status ().local_endpoint ());
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while ((source.admitted_peer_count () == 0 || target.admitted_peer_count () == 0)
           && std::chrono::steady_clock::now () < deadline) {
        source.dispatch_ready ([] (const auto &, const auto &, auto) {}).result ().value ();
        target.dispatch_ready ([] (const auto &, const auto &, auto) {}).result ().value ();
        std::this_thread::yield ();
    }
    ASSERT_EQ (1u, source.admitted_peer_count ());
    ASSERT_EQ (1u, target.admitted_peer_count ());
    const auto actor = detail::actor_ref_access_t::make (node_rid_t::from_string ("wire-target"),
                                                         "player", "actor", 1);
    runtime::protocol::actor_route_fence_t fence{
      "actor", 1, target_state->routing_id->to_bytes (), target.status ().lifecycle_generation (),
      1,       1};
    auto metadata = detail::stream_metadata_t{}.values ();
    if (ordered_relay)
        metadata.emplace (std::string (detail::bound_session_relay_sequence_key), "1");
    detail::stream_header_t header (detail::stream_message_kind_t::send, stream_codec_t::json,
                                    detail::stream_header_flags_t::none, std::nullopt, "WireSend",
                                    detail::stream_metadata_t (std::move (metadata)));
    auto pending = source.relay_application_actor (actor, header, zlink::message_t::from ("{}"),
                                                   std::chrono::milliseconds (100),
                                                   await_remote_admission, std::nullopt, fence);
    // The remote application queue is deliberately not drained and emits no reply.
    const auto sent = detail::request_task_access_t::physical_reply (pending).result ();
    EXPECT_TRUE (sent) << (sent.error () ? sent.error ()->what () : "");
    source.stop ();
    target.stop ();
}

INSTANTIATE_TEST_SUITE_P (All,
                          ActorSendRelayAdmission,
                          ::testing::Combine (::testing::Bool (), ::testing::Bool ()));
