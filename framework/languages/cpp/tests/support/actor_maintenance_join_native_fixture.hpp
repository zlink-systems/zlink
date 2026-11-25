/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "actor_maintenance_join_observations.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <array>
#include <future>

namespace zlink::framework::test::actor_maintenance_join
{
// Exercises the same raw admission boundary without a discovery scheduling race.
struct relay_peer_pair_t
{
    using host_type = fw::runtime::host::public_host_runtime_t;
    using descriptor_type = fw::runtime::mesh::service_node_descriptor_t;
    fw::runtime::in_memory_location_repository_t source_store;
    fw::runtime::live_location_reader_t source_reader{source_store};
    std::shared_ptr<host_type> source, target;
    std::array<std::future<void>, 2> hosts_destroyed;
    descriptor_type source_wire, target_wire;
    std::shared_ptr<const fw::mesh_node_descriptor_t> original;
    std::shared_ptr<const fw::location_owner_token_t> lease =
      std::make_shared<const fw::location_owner_token_t> (
        fw::location_owner_token_t{"target-owner", 1});

    explicit relay_peer_pair_t (
      fw::runtime::mesh::service_object_role_t source_role =
        fw::runtime::mesh::service_object_role_t::server,
      fw::runtime::mesh::service_object_role_t target_role =
        fw::runtime::mesh::service_object_role_t::server,
      fw::runtime::stateful::accepted_record_authority_resolver_t authority = {});
    ~relay_peer_pair_t ();
    static std::shared_ptr<const fw::mesh_node_descriptor_t> discovery (const descriptor_type &wire,
                                                                        const char *owner);
    // The source original a raw receive turn on the target reads for the source peer.
    std::optional<fw::runtime::mesh::request_source_t> accepted_source (std::uint64_t generation);
    void pump ();
    bool ready ();
    bool await_ready ();
};

struct native_intake_fixture_t
{
    static constexpr auto source_id = "boundary-source";
    std::vector<std::uint8_t> rid = zlink::routing_id_t::from ("intake-boundary").to_bytes ();
    fw::runtime::in_memory_location_repository_t source_store;
    fw::runtime::live_location_reader_t source_reader{source_store};
    fw::runtime::stateful::accepted_record_authority_resolver_t resolver;
    std::shared_ptr<zlink::context_t> core_context = std::make_shared<zlink::context_t> ();
    std::shared_ptr<fw::runtime::host::public_host_runtime_t> host;
    explicit native_intake_fixture_t (
      fw::dispatch_options_t dispatch = {},
      fw::runtime::stateful::accepted_record_authority_resolver_t authority = {});
    ~native_intake_fixture_t ();
    fw::runtime::mesh::service_mailbox_record_t
    actor_record (std::uint64_t sequence, std::uint64_t authority_generation = 1) const;
};
} // namespace zlink::framework::test::actor_maintenance_join
