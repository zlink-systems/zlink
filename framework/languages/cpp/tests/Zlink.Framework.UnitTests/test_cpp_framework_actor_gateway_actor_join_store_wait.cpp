/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <vector>
#include <mutex>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace zlink::framework::test::actor_gateway
{
/* A Location Store whose reads complete on the provider's own thread, as a
 * networked provider does. */
class provider_thread_location_store_t final : public zlink::framework::location_store_t
{
  public:
    ~provider_thread_location_store_t () override
    {
        for (auto &completer : _completers)
            completer.join ();
    }

    zlink::framework::task_t<zlink::framework::store_read_result_t>
    read (zlink::framework::store_key_t key) override
    {
        using namespace zlink::framework;
        auto completion = std::make_shared<task_completion_source_t<store_read_result_t>> ();
        auto pending = completion->task ();
        auto value = inner.read (std::move (key)).result ();
        std::lock_guard lock (_mutex);
        _completers.emplace_back ([completion, value] { completion->complete (value); });
        return pending;
    }
    zlink::framework::task_t<zlink::framework::store_scan_result_t>
    scan (zlink::framework::store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    zlink::framework::task_t<zlink::framework::store_write_result_t>
    write (zlink::framework::store_write_request_t request) override
    {
        return inner.write (std::move (request));
    }

    zlink::framework::runtime::in_memory_location_store_t inner;

  private:
    std::mutex _mutex;
    std::vector<std::thread> _completers;
};

/* Execution gate §1, §13: the actorJoin admission reads the Authority row
 * from an infrastructure thread. That read must finish while every handler
 * executor worker is busy; before #1380's follow-up its Store continuations
 * were queued behind those workers and the admission never returned. */
int remote_actor_join_store_read_does_not_wait_for_handler_workers ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using namespace std::chrono_literals;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-join-infrastructure");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "actor-join-infrastructure");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    provider_thread_location_store_t provider;
    runtime::provider_location_repository_t repository (provider);
    spot_node_runtime_t spots (node);
    service_collection_t services;
    services.add_factory<runtime::live_location_reader_t> (
      [&repository] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (repository);
      },
      service_lifetime_t::singleton);
    auto provider_services = services.build_provider ();
    spots.bind_service_provider (provider_services);

    runtime::configure_handler_coroutine_executor (1);
    runtime::install_host_context_hooks ();
    std::promise<void> worker_entered;
    std::promise<void> release_worker;
    auto released = release_worker.get_future ().share ();
    runtime::handler_coroutine_executor ().post_native_continuation ([&worker_entered, released] {
        worker_entered.set_value ();
        released.wait ();
    });
    worker_entered.get_future ().wait ();

    std::promise<std::pair<framework_error_kind_t, framework_error_kind_t>> outcome;
    auto finished = outcome.get_future ();
    std::thread infrastructure ([&] {
        const auto admitted =
          spots
            .admit_remote_actor_to_spot ("infrastructure-store-read",
                                         test_actor_ref ("actor-owner", "", "absent-actor", 17),
                                         spot_id_t ("source-spot"), spot_id_t ("target-spot"),
                                         zlink::message_t{}, 1, 2, 23, 19, 29, true)
            .result ();
        runtime::protocol::spot_route_fence_t fence;
        fence.spot_id = "absent-spot";
        fence.object_generation = 3;
        fence.target_node_routing_id = {1};
        fence.target_node_generation = 5;
        fence.authority_owner_generation = 7;
        fence.owner_lease_generation = 9;
        const auto target = spots.resolve_wire_actor_join_target (fence);
        outcome.set_value (
          {admitted ? framework_error_kind_t::internal_failure : admitted.error_kind (),
           target ? framework_error_kind_t::internal_failure : target.error_kind ()});
    });
    if (finished.wait_for (10s) != std::future_status::ready) {
        // The infrastructure thread waits for a continuation queued behind the
        // held worker; the executor cannot drain, so end the process.
        std::cerr << "infrastructure Store read waited for a handler worker\n";
        std::_Exit (1);
    }
    release_worker.set_value ();
    infrastructure.join ();
    runtime::shutdown_handler_coroutine_executor ();
    const auto [admission, target] = finished.get ();
    if (admission != framework_error_kind_t::not_found)
        return 2;
    return target == framework_error_kind_t::not_found ? 0 : 3;
}
} // namespace zlink::framework::test::actor_gateway
