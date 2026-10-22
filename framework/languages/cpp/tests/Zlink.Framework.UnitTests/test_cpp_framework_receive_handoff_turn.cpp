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
#include <cassert>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>

namespace fw = zlink::framework;
namespace rt = fw::runtime;
using namespace std::chrono_literals;
namespace zlink::framework::runtime
{
struct offload_executor_test_access_t
{
    static bool application_pending (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        return !executor._queue.empty () || executor._active > executor._role_active;
    }
    static std::size_t idle (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        return executor._idle_workers;
    }
};
}
namespace
{
std::mutex observation_mutex;
std::thread::id received_on;
bool pending_handoff = false;
bool reject_handoff = false;
std::promise<std::pair<std::thread::id, bool>> assigned;
std::promise<std::thread::id> handled;
std::atomic_bool handler_entered{false};
std::atomic_bool handoff_observed{false};
std::atomic_bool direct_blocked{false};
std::atomic_size_t posted_before_handoff{0};
std::promise<void> parallel_started;
std::shared_future<void> release_direct;
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
        const bool hold = !reject_handoff && handoff_observed.load ();
        direct_blocked = hold;
        handler_entered = true;
        handled.set_value (std::this_thread::get_id ());
        if (hold)
            release_direct.wait ();
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
        constexpr char marker[] = "handoff-probe";
        for (std::size_t index = 0; index < *count; ++index) {
            const auto *data = static_cast<const char *> (zlink_msg_data (&parts[index]));
            const auto bytes = zlink_msg_size (&parts[index]);
            if (bytes >= sizeof (marker) - 1
                && std::string_view (data, bytes).find (marker) != std::string_view::npos) {
                std::lock_guard lock (observation_mutex);
                received_on = std::this_thread::get_id ();
                pending_handoff = true;
            }
        }
    }
    return result;
}
extern "C" bool
real_assign (rt::offload_executor_t *, std::shared_ptr<rt::offload_executor_t::role_t>) __asm__ (
  "__real__ZN5zlink9framework7runtime18offload_executor_t15try_assign_roleESt10shared_ptrINS2_"
  "6role_tEE");
extern "C" bool
wrapped_assign (rt::offload_executor_t *executor,
                std::shared_ptr<rt::offload_executor_t::role_t>
                  role) __asm__ ("__wrap__ZN5zlink9framework7runtime18offload_executor_t15try_"
                                 "assign_roleESt10shared_ptrINS2_6role_tEE");
extern "C" bool wrapped_assign (rt::offload_executor_t *executor,
                                std::shared_ptr<rt::offload_executor_t::role_t> role)
{
    bool watched;
    {
        std::lock_guard lock (observation_mutex);
        watched = pending_handoff && received_on == std::this_thread::get_id ();
        if (watched)
            pending_handoff = false;
    }
    if (watched
        && (handler_entered.load ()
            || rt::offload_executor_test_access_t::application_pending (*executor))) {
        ++posted_before_handoff;
        // The original receive turn has already published this owner. Let
        // its posted worker enter before the receive resource can continue.
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (!handler_entered.load ()) {
            assert (std::chrono::steady_clock::now () < deadline);
            std::this_thread::yield ();
        }
    }
    if (watched && !reject_handoff) {
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (rt::offload_executor_test_access_t::idle (*executor) == 0) {
            assert (std::chrono::steady_clock::now () < deadline);
            std::this_thread::yield ();
        }
    }
    const bool accepted =
      watched && reject_handoff ? false : real_assign (executor, std::move (role));
    if (watched) {
        handoff_observed = true;
        assigned.set_value ({std::this_thread::get_id (), accepted});
    }
    return accepted;
}
int main ()
{
    bool correct = true;
    for (const bool rejected : {false, true}) {
        fw::completion_owner_t completion;
        assigned = {};
        handled = {};
        handler_entered = false;
        handoff_observed = false;
        direct_blocked = false;
        posted_before_handoff = 0;
        parallel_started = {};
        std::promise<void> release_handler;
        release_direct = release_handler.get_future ().share ();
        auto parallel = parallel_started.get_future ();
        reject_handoff = rejected;
        auto assignment = assigned.get_future ();
        auto invocation = handled.get_future ();
        auto registration =
          fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("handoff-turn");
        registration->core_context = std::make_shared<zlink::context_t> ();
        registration->listen_endpoint = "tcp://127.0.0.1:0";
        registration->listen_port.reset ();
        registration->routing_id = zlink::routing_id_t::from ("target");
        registration->handlers.on_send<handler_t, probe_t> ("handoff-turn", "HandoffProbe",
                                                            &handler_t::handle);
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
        rt::mesh_node_host_service_t service (std::make_shared<rt::runtime_failure_collector_t> (),
                                              {registration}, serializers);
        service.start (provider).result ().value ();
        const auto target = service.nodes ().front ();
        auto source_state =
          fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("handoff-turn");
        source_state->core_context = std::make_shared<zlink::context_t> ();
        source_state->listen_endpoint = "tcp://127.0.0.1:0";
        source_state->listen_port.reset ();
        source_state->routing_id = zlink::routing_id_t::from ("source");
        fw::detail::mesh_node_runtime_t source (source_state);
        source.start ();
        source.connect_peer (*registration->routing_id, target->status ().local_endpoint ());
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (source.admitted_peer_count () == 0 || target->admitted_peer_count () == 0) {
            assert (std::chrono::steady_clock::now () < deadline);
            source.dispatch_ready ([] (const auto &, const auto &, auto) {}).result ().value ();
            std::this_thread::yield ();
        }
        rt::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (rt::messaging::message_kind_t::command, "handoff-turn",
                                             "HandoffProbe");
        auto parts = codec.encode_envelope_parts (header, probe_t{"handoff-probe"}, serializers);
        assert (
          source
            .send_to_node (*registration->routing_id, parts.items (), std::vector<std::uint8_t>{})
            .result ()
            .value ()
          == zlink::submit_result_t::ok);
        assert (assignment.wait_for (5s) == std::future_status::ready);
        assert (invocation.wait_for (5s) == std::future_status::ready);
        const auto [receiver, accepted] = assignment.get ();
        const auto handler = invocation.get ();
        const bool same = receiver == handler;
        if (!rejected) {
            auto completed = std::make_shared<std::promise<void>> ();
            auto terminal = completed->get_future ();
            const auto operation =
              target->native_node ()
                .transport ()
                .register_local_operation_async (
                  rt::foundation::operation_registry_t::clock_t::time_point::min (),
                  [completed] (auto outcome, zlink::message_t) {
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
            correct = correct && operation && parallel_send
                      && parallel_send.value () == zlink::submit_result_t::ok && other_owner
                      && completion_ready && direct_blocked.load ();
            std::cout << "blocked_direct=" << direct_blocked.load ()
                      << " independent_owner=" << other_owner
                      << " independent_completion=" << completion_ready << '\n';
        }
        std::cout << "handoff rejected=" << rejected << " accepted=" << accepted
                  << " same_tid=" << same
                  << " posted_before_handoff=" << posted_before_handoff.load () << '\n';
        correct = correct && accepted == !rejected && same == !rejected
                  && posted_before_handoff.load () == 0;
        source.stop ();
        service.request_stop ();
        service.stop ();
    }
    return correct ? 0 : 1;
}
