/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>

int main ()
{
    zlink::framework::completion_owner_t completion_owner;
    using namespace zlink::framework;
    using namespace std::chrono_literals;
    auto registration =
      test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("completion-only-mesh");
    registration->core_context = std::make_shared<zlink::context_t> ();
    registration->listen_endpoint = "tcp://127.0.0.1:0";
    registration->listen_port.reset ();
    registration->routing_id = zlink::routing_id_t::from ("completion-only-node");

    serializer_registry_t serializers;
    service_collection_t services;
    auto owned_store = std::make_unique<runtime::in_memory_location_repository_t> ();
    auto &store = *owned_store;
    services.add_singleton<location_repository_t> (
      std::unique_ptr<location_repository_t> (owned_store.release ()));
    services.add_factory<runtime::live_location_reader_t> (
      [] (service_provider_t &provider) {
          return std::make_unique<runtime::live_location_reader_t> (
            provider.get_required<location_repository_t> ());
      },
      service_lifetime_t::singleton);
    services.add_singleton<runtime::location_runtime_t> (
      std::make_unique<runtime::location_runtime_t> (store));
    auto resolvers = std::make_shared<runtime::store_location_resolvers_t> (store);
    services.add_factory<runtime::spot_address_resolver_t> (
      [resolvers] (service_provider_t &) {
          return std::static_pointer_cast<runtime::spot_address_resolver_t> (resolvers);
      },
      service_lifetime_t::singleton);
    services.add_factory<runtime::actor_address_resolver_t> (
      [resolvers] (service_provider_t &) {
          return std::static_pointer_cast<runtime::actor_address_resolver_t> (resolvers);
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    provider.get_required<runtime::location_runtime_t> ().start (*registration->routing_id);
    runtime::mesh_node_host_service_t service (
      std::make_shared<runtime::runtime_failure_collector_t> (), {registration}, serializers);
    service.start (provider).result ().value ();
    const auto node = service.nodes ().front ();

    std::promise<runtime::foundation::operation_terminal_t> completed;
    auto terminal = completed.get_future ();
    // Expiry contributes an E5 completion and no ordinary receive record.
    const auto operation =
      node->native_node ()
        .transport ()
        .register_local_operation_async (
          runtime::foundation::operation_registry_t::clock_t::time_point::min (),
          [&] (runtime::foundation::operation_terminal_t result, zlink::message_t) {
              assert (detail::current_completion_owner () != nullptr);
              completed.set_value (result);
          })
        .result ()
        .value ();
    assert (operation);
    assert (terminal.wait_for (1s) == std::future_status::ready);
    assert (terminal.get () == runtime::foundation::operation_terminal_t::timed_out);
    assert (node->native_node ().transport ().pending_operation_count () == 0);
    std::promise<void> callback_entered, release_callback, stop_started;
    auto callback_running = callback_entered.get_future ();
    auto released = release_callback.get_future ();
    std::atomic_size_t callbacks{0};
    assert (node->native_node ()
              .transport ()
              .register_local_operation_async (
                runtime::foundation::operation_registry_t::clock_t::time_point::min (),
                [&] (runtime::foundation::operation_terminal_t result, zlink::message_t) {
                    assert (detail::current_completion_owner () != nullptr);
                    assert (result == runtime::foundation::operation_terminal_t::timed_out);
                    callback_entered.set_value ();
                    assert (released.wait_for (5s) == std::future_status::ready);
                    ++callbacks;
                })
              .result ()
              .value ());
    assert (callback_running.wait_for (1s) == std::future_status::ready);
    std::promise<runtime::foundation::operation_terminal_t> shutdown_terminal;
    auto shutdown_delivered = shutdown_terminal.get_future ();
    assert (node->native_node ()
              .transport ()
              .register_local_operation_async (
                runtime::foundation::operation_registry_t::clock_t::time_point::max (),
                [&] (runtime::foundation::operation_terminal_t result, zlink::message_t) {
                    assert (detail::current_completion_owner () != nullptr);
                    ++callbacks;
                    shutdown_terminal.set_value (result);
                })
              .result ()
              .value ());
    service.request_stop ();
    auto stopping = std::async (std::launch::async, [&] {
        stop_started.set_value ();
        service.stop ();
    });
    stop_started.get_future ().get ();
    release_callback.set_value ();
    assert (stopping.wait_for (5s) == std::future_status::ready);
    stopping.get ();
    assert (shutdown_delivered.wait_for (0s) == std::future_status::ready);
    assert (shutdown_delivered.get () == runtime::foundation::operation_terminal_t::shutdown);
    assert (callbacks == 2 && node->pending_transport_operations () == 0);
}
