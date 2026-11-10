/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include <zlink.h>
#include <array>
#include <cassert>
#include <future>
#include <iostream>
#include <mutex>

namespace fw = zlink::framework;
namespace rt = fw::runtime;
using namespace std::chrono_literals;
namespace
{
std::mutex observation_mutex;
std::thread::id received_on;
std::promise<std::thread::id> handled;
std::atomic_bool handler_blocked{false};
constexpr std::size_t queued_records = 128;
std::atomic_size_t received_queued{0};
std::atomic_size_t handled_queued{0};
std::array<std::atomic_bool, queued_records> queued_seen{};
std::promise<void> queue_drained;
std::promise<void> parallel_started;
std::shared_future<void> release_handler_turn;
struct probe_t
{
    std::string value;
};
struct handler_t
{
    void handle (const probe_t &probe, const fw::route_message_context_t &)
    {
        if (probe.value == "parallel-probe") {
            parallel_started.set_value ();
            return;
        }
        if (probe.value.starts_with ("queued-probe-")) {
            const auto index = std::stoul (probe.value.substr (13));
            assert (index < queued_records && !queued_seen[index].exchange (true));
            if (handled_queued.fetch_add (1) + 1 == queued_records)
                queue_drained.set_value ();
            return;
        }
        handler_blocked = true;
        handled.set_value (std::this_thread::get_id ());
        release_handler_turn.wait ();
    }
};
}
extern "C" zlink_recv_result_t __real_zlink_router_recv (void *,
                                                         const zlink_routing_id_t **,
                                                         zlink_reply_token_t *,
                                                         zlink_msg_t *,
                                                         size_t,
                                                         size_t *,
                                                         zlink_recv_flags_t);
extern "C" zlink_recv_result_t __wrap_zlink_router_recv (void *socket,
                                                         const zlink_routing_id_t **rid,
                                                         zlink_reply_token_t *reply,
                                                         zlink_msg_t *parts,
                                                         size_t capacity,
                                                         size_t *count,
                                                         zlink_recv_flags_t flags)
{
    const auto result =
      __real_zlink_router_recv (socket, rid, reply, parts, capacity, count, flags);
    if (result == ZLINK_RECV_OK) {
        constexpr char marker[] = "owner-dispatch-probe";
        for (std::size_t index = 0; index < *count; ++index) {
            const auto *data = static_cast<const char *> (zlink_msg_data (&parts[index]));
            const auto bytes = zlink_msg_size (&parts[index]);
            if (std::string_view (data, bytes).find ("queued-probe-") != std::string_view::npos)
                ++received_queued;
            if (bytes >= sizeof (marker) - 1
                && std::string_view (data, bytes).find (marker) != std::string_view::npos) {
                std::lock_guard lock (observation_mutex);
                received_on = std::this_thread::get_id ();
            }
        }
    }
    return result;
}
int main ()
{
    bool correct = true;
    {
        fw::completion_owner_t completion;
        handled = {};
        received_queued = 0;
        handled_queued = 0;
        for (auto &seen : queued_seen)
            seen.store (false);
        queue_drained = {};
        auto drained = queue_drained.get_future ();
        handler_blocked = false;
        parallel_started = {};
        std::promise<void> release_handler;
        release_handler_turn = release_handler.get_future ().share ();
        auto parallel = parallel_started.get_future ();
        auto invocation = handled.get_future ();
        auto registration =
          fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> (
            "mailbox-owner-dispatch");
        registration->core_context = std::make_shared<zlink::context_t> ();
        registration->listen_endpoint = "tcp://127.0.0.1:0";
        registration->listen_port.reset ();
        registration->routing_id = zlink::routing_id_t::from ("target");
        registration->handlers.on_send<handler_t, probe_t> ("mailbox-owner-dispatch",
                                                            "HandoffProbe", &handler_t::handle);
        registration->handlers.on_send<handler_t, probe_t> ("parallel", "HandoffProbe",
                                                            &handler_t::handle);
        registration->channels.emplace (
          "parallel", fw::detail::mesh_channel_registration_t{100, {}, true, true});
        fw::serializer_registry_t serializers;
        serializers.add<probe_t> (
          [] (const probe_t &value) { return fw::encoded_payload_t::from_string (value.value); },
          [] (const fw::encoded_payload_t &value) { return probe_t{value.to_string ()}; });
        fw::service_collection_t services;
        services.add_singleton<handler_t> (std::make_unique<handler_t> ());
        auto owned = std::make_unique<rt::in_memory_location_repository_t> ();
        auto &store = *owned;
        services.add_singleton<fw::location_repository_t> (
          std::unique_ptr<fw::location_repository_t> (owned.release ()));
        services.add_factory<rt::live_location_reader_t> (
          [] (fw::service_provider_t &provider) {
              return std::make_unique<rt::live_location_reader_t> (
                provider.get_required<fw::location_repository_t> ());
          },
          fw::service_lifetime_t::singleton);
        services.add_singleton<rt::location_runtime_t> (
          std::make_unique<rt::location_runtime_t> (store));
        auto resolvers = std::make_shared<rt::store_location_resolvers_t> (store);
        services.add_factory<rt::spot_address_resolver_t> (
          [resolvers] (auto &) {
              return std::static_pointer_cast<rt::spot_address_resolver_t> (resolvers);
          },
          fw::service_lifetime_t::singleton);
        services.add_factory<rt::actor_address_resolver_t> (
          [resolvers] (auto &) {
              return std::static_pointer_cast<rt::actor_address_resolver_t> (resolvers);
          },
          fw::service_lifetime_t::singleton);
        auto provider = services.build_provider ();
        provider.get_required<rt::location_runtime_t> ().start (*registration->routing_id);
        rt::mesh_node_host_service_t service (
          std::make_shared<rt::runtime_failure_collector_t> (), {registration}, serializers, {}, {},
          std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{
            fw::application_job_queue_profile_t::balanced, std::uint32_t{512}, 1, 512}));
        service.start (provider).result ().value ();
        const auto target = service.nodes ().front ();
        auto source_state =
          fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> (
            "mailbox-owner-dispatch");
        source_state->core_context = std::make_shared<zlink::context_t> ();
        source_state->listen_endpoint = "tcp://127.0.0.1:0";
        source_state->listen_port.reset ();
        source_state->routing_id = zlink::routing_id_t::from ("source");
        fw::detail::mesh_node_runtime_t source (source_state);
        source.start ().result ().value ();
        source.connect_peer (*registration->routing_id, target->status ().local_endpoint ());
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (source.admitted_peer_count () == 0 || target->admitted_peer_count () == 0) {
            assert (std::chrono::steady_clock::now () < deadline);
            source.dispatch_ready ([] (const auto &, const auto &, auto, auto) {})
              .result ()
              .value ();
            std::this_thread::yield ();
        }
        rt::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (rt::messaging::message_kind_t::command,
                                             "mailbox-owner-dispatch", "HandoffProbe");
        auto parts =
          codec.encode_envelope_parts (header, probe_t{"owner-dispatch-probe"}, serializers);
        assert (
          source
            .send_to_node (*registration->routing_id, parts.items (), std::vector<std::uint8_t>{})
            .result ()
            .value ()
          == zlink::submit_result_t::ok);
        assert (invocation.wait_for (5s) == std::future_status::ready);
        std::thread::id receiver;
        {
            std::lock_guard lock (observation_mutex);
            receiver = received_on;
        }
        const auto handler = invocation.get ();
        const bool same = receiver == handler;
        {
            for (std::size_t index = 0; index < queued_records; ++index) {
                auto queued = codec.encode_envelope_parts (
                  header, probe_t{"queued-probe-" + std::to_string (index)}, serializers);
                assert (source
                          .send_to_node (*registration->routing_id, queued.items (),
                                         std::vector<std::uint8_t>{})
                          .result ()
                          .value ()
                        == zlink::submit_result_t::ok);
            }
            const auto receive_deadline = std::chrono::steady_clock::now () + 5s;
            while (received_queued.load () < queued_records) {
                assert (std::chrono::steady_clock::now () < receive_deadline);
                std::this_thread::yield ();
            }
            // S02: the same Node must progress while its earlier handler blocks.
            const bool same_owner = drained.wait_for (5s) == std::future_status::ready;
            correct = correct && same_owner;
            std::cout << "blocked_handler_same_node_progress=" << same_owner << '\n';
            auto completed = std::make_shared<std::promise<void>> ();
            auto terminal = completed->get_future ();
            const auto operation =
              target->native_node ()
                .transport ()
                .register_local_operation_async (
                  rt::foundation::operation_registry_t::clock_t::time_point::min (),
                  [completed] (auto outcome, zlink::framework::result_t<zlink::message_t>) {
                      assert (outcome == rt::foundation::operation_terminal_t::timed_out);
                      completed->set_value ();
                  })
                .result ()
                .value ();
            auto parallel_header = codec.create_envelope (rt::messaging::message_kind_t::command,
                                                          "parallel", "HandoffProbe");
            auto parallel_parts =
              codec.encode_envelope_parts (parallel_header, probe_t{"parallel-probe"}, serializers);
            const auto parallel_send =
              source
                .send_to_channel ("parallel", parallel_parts.items (), std::vector<std::uint8_t>{})
                .result ();
            const bool other_owner = parallel.wait_for (5s) == std::future_status::ready;
            const bool completion_ready = terminal.wait_for (5s) == std::future_status::ready;
            release_handler.set_value ();
            assert (drained.wait_for (5s) == std::future_status::ready);
            correct = correct && operation && parallel_send
                      && parallel_send.value () == zlink::submit_result_t::ok && other_owner
                      && completion_ready && handler_blocked.load ();
            std::cout << "blocked_handler=" << handler_blocked.load ()
                      << " independent_owner=" << other_owner
                      << " independent_completion=" << completion_ready << '\n';
        }
        std::cout << "queued_records=" << handled_queued.load ()
                  << " handler_on_receive_thread=" << same << '\n';
        correct = correct && !same && handled_queued.load () == queued_records;
        source.stop ();
        service.request_stop ();
        service.stop ();
    }
    return correct ? 0 : 1;
}
