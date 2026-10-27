/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/channel_packet_dispatcher.hpp"
#include "runtime/dispatch/offload_executor.hpp"

#include <zlink/framework.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <future>
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
            auto retained = worker.retain_internal_work ();
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
