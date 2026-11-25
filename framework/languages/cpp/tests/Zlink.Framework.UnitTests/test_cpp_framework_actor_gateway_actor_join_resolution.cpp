/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
class actor_join_authority_store_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    std::optional<zlink::framework::authority_snapshot_t> snapshot;
    bool unreadable = false;

    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority_core (zlink::framework::authority_key_t, std::stop_token) override
    {
        if (unreadable) {
            return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
              zlink::framework::result_t<zlink::framework::authority_read_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "Location Store is unavailable"));
        }
        if (snapshot) {
            return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
              zlink::framework::result_t<zlink::framework::authority_read_result_t>::success (
                zlink::framework::authority_read_result_t{*snapshot}));
        }
        return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
          zlink::framework::result_t<zlink::framework::authority_read_result_t>::success (
            zlink::framework::authority_read_result_t{
              zlink::framework::authority_missing_t{std::chrono::system_clock::now ()}}));
    }
};

int remote_actor_join_resolves_store_type_and_reports_typed_terminals ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    enum class scenario_t
    {
        matching,
        no_store,
        fence_mismatch,
        missing,
        unreadable,
        forged,
        no_factory,
        reject_while_source_remote,
        accept_while_source_remote
    };
    const auto run = [] (scenario_t scenario,
                         std::atomic_int &admission_calls) -> result_t<spot_actor_join_result_t> {
        serializer_registry_t serializers;
        auto node = zlink::framework::test::runtime_failure_fixture<
          zlink::framework::detail::spot_node_builder_state_t> ("actor-join-target");
        node->worker_executor =
          std::make_shared<runtime::offload_executor_t> (1, "actor-join-store-resolution");
        node->channel_runtime =
          zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
        node->channel_runtime->serializers = &serializers;

        auto spot = std::make_shared<spot_context_state_t> ();
        spot->node = node;
        spot->node_rid = node_rid_t::from_string ("actor-join-target");
        spot->spot_id = spot_id_t ("actor-join-target-spot");
        spot->spot_name = "actor-join-target";
        spot->spot_instance = std::make_shared<int> (1);
        spot->channel_runtime =
          zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
        spot->channel_runtime->serializers = &serializers;
        spot->serial_executor = node->worker_executor;
        node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

        spot_actor_admission_callbacks_t callbacks;
        callbacks.join = [&admission_calls, scenario] (void *, std::string_view,
                                                       const zlink::message_t &,
                                                       serializer_registry_t &) {
            admission_calls.fetch_add (1, std::memory_order_acq_rel);
            return scenario == scenario_t::reject_while_source_remote
                     ? spot_actor_join_result_t::reject ()
                     : spot_actor_join_result_t::accept ();
        };
        spot->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

        if (scenario != scenario_t::no_factory) {
            spot_node_builder_state_t::actor_factory_registration_t factory;
            factory.actor_type = std::type_index (typeid (int));
            factory.create_instance = [] (std::string) { return std::make_shared<int> (1); };
            factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
            node->actor_factories.emplace ("StoreActor", std::move (factory));
        }

        const auto stored_actor =
          test_actor_ref ("actor-owner", "StoreActor", "store-resolved-actor", 17);
        auto store = std::make_shared<actor_join_authority_store_t> ();
        if (scenario != scenario_t::missing) {
            store->snapshot = authority_snapshot_t{
              .store_version = "v1",
              .payload = runtime::encode_actor_authority_payload (stored_actor, "source-spot", 1),
              .object_generation = 17,
              .authority_owner_generation = 23,
              .owner = location_owner_token_t{"source-owner", 29},
              .store_now = std::chrono::system_clock::now (),
              .allocation = {.state = placement_allocation_state_t::active,
                             .object_kind = placement_object_kind_t::actor,
                             .stable_type = "StoreActor",
                             .target = {.mesh_name = "mesh",
                                        .node_rid = node_rid_t::from_string ("actor-owner"),
                                        .node_lifecycle_generation = 19,
                                        .owner = location_owner_token_t{"source-owner", 29}}}};
            if (scenario == scenario_t::fence_mismatch)
                store->snapshot->allocation.target.node_lifecycle_generation = 21;
        }
        store->unreadable = scenario == scenario_t::unreadable;

        spot_node_runtime_t spots (node);
        if (scenario != scenario_t::no_store) {
            service_collection_t services;
            services.add_factory<runtime::live_location_reader_t> (
              [store] (service_provider_t &) {
                  return std::make_unique<runtime::live_location_reader_t> (*store);
              },
              service_lifetime_t::singleton);
            auto provider = services.build_provider ();
            spots.bind_service_provider (provider);
        }

        const auto wire_actor = test_actor_ref (
          "actor-owner", scenario == scenario_t::forged ? "ForgedActor" : "StoreActor",
          "store-resolved-actor", 17);
        if (scenario == scenario_t::reject_while_source_remote
            || scenario == scenario_t::accept_while_source_remote) {
            if (!node->actor_transfer_coordinator->try_begin_source_remote (
                  "StoreActor:store-resolved-actor", "previous-source-transfer"))
                return result_t<spot_actor_join_result_t>::failure (
                  framework_error_kind_t::internal_failure, "source transfer setup failed");
        }
        const auto result =
          spots
            .admit_remote_actor_to_spot (
              "store-resolution-" + std::to_string (static_cast<int> (scenario)), wire_actor,
              spot_id_t ("source-spot"), spot->spot_id, zlink::message_t{}, 1, 2, 23, 19, 29)
            .result ();
        spots.release_native_handles ();
        return result;
    };

    std::atomic_int admission_calls{0};
    const auto matching = run (scenario_t::matching, admission_calls);
    if (!matching || !matching.value ().accepted || admission_calls.load () != 1)
        return 1;

    const auto expect_terminal =
      [&run, &admission_calls] (scenario_t scenario, framework_error_kind_t expected, int failure) {
          const auto result = run (scenario, admission_calls);
          return !result && result.error_kind () == expected ? 0 : failure;
      };
    if (const auto mismatch =
          expect_terminal (scenario_t::no_store, framework_error_kind_t::unavailable, 2);
        mismatch != 0)
        return mismatch;
    if (const auto mismatch =
          expect_terminal (scenario_t::fence_mismatch, framework_error_kind_t::protocol_error, 3);
        mismatch != 0)
        return mismatch;
    if (const auto missing =
          expect_terminal (scenario_t::missing, framework_error_kind_t::not_found, 4);
        missing != 0)
        return missing;
    if (const auto unreadable =
          expect_terminal (scenario_t::unreadable, framework_error_kind_t::unavailable, 5);
        unreadable != 0)
        return unreadable;
    if (const auto forged =
          expect_terminal (scenario_t::forged, framework_error_kind_t::type_mismatch, 6);
        forged != 0)
        return forged;
    if (const auto no_factory =
          expect_terminal (scenario_t::no_factory, framework_error_kind_t::rejected, 7);
        no_factory != 0)
        return no_factory;
    const auto rejected = run (scenario_t::reject_while_source_remote, admission_calls);
    if (!rejected || rejected.value ().accepted)
        return 9;
    if (const auto conflict = expect_terminal (scenario_t::accept_while_source_remote,
                                               framework_error_kind_t::unavailable, 10);
        conflict != 0)
        return conflict;
    return admission_calls.load () == 3 ? 0 : 8;
}
} // namespace zlink::framework::test::actor_gateway
