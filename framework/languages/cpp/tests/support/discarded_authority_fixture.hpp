/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "location_reclaim_clock.hpp"
#include <gtest/gtest.h>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace std::chrono_literals;

namespace
{
class observed_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (before_write) {
            auto callback = std::exchange (before_write, {});
            co_await callback ();
        }
        writes.push_back (request);
        co_return co_await inner.write (std::move (request));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        if (fail_scan)
            throw framework_exception_t (framework_error_kind_t::unavailable, "membership scan");
        if (expire_scan && request.cursor)
            co_return store_scan_expired_t{};
        scan_prefixes.push_back (request.prefix);
        request.limit = 1;
        auto result = co_await inner.scan (std::move (request));
        if (const auto *page = std::get_if<store_scan_page_t> (&result))
            scanned_items += page->items.size ();
        co_return result;
    }
    in_memory_location_store_t inner;
    std::vector<store_write_request_t> writes;
    std::vector<std::string> scan_prefixes;
    std::size_t scanned_items = 0;
    std::function<task_t<void> ()> before_write;
    bool fail_scan = false;
    bool expire_scan = false;
};

class DiscardedAuthority : public ::testing::TestWithParam<bool>
{
  protected:
    observed_store_t opaque;
    provider_location_repository_t provider{opaque};
    in_memory_location_repository_t memory;
    location_repository_t *repository = nullptr;
    location_reclaim_test_access_t clock;
    location_owner_token_t owner;
    mesh_node_descriptor_t descriptor;

    void SetUp () override
    {
        repository = GetParam () ? static_cast<location_repository_t *> (&provider) : &memory;
        clock.bind (*repository);
        repository->configure_reclaim_margin (0ms);
        seed_owner ();
    }

    void seed_owner ()
    {
        owner = std::get<owner_lease_claimed_t> (
                  repository->claim_owner_lease ("discard-owner", 1h).result ().value ())
                  .token;
        descriptor.mesh_name = "discard-mesh";
        descriptor.rid = zlink::routing_id_t::from ("discard-node");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.security_identity = "discard-node";
        descriptor.entry_spot_id = "discard-entry";
        descriptor.capacity.actors.limit = 20;
        descriptor.capacity.spots.limit = 20;
        descriptor.capacity.spot_types = {
          {placement_object_kind_t::user_spot, "room", {0, 0, 20}},
          {placement_object_kind_t::instance_spot, "instance", {0, 0, 20}}};
        descriptor.object_capabilities = {
          {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
          {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::recreate, false,
           20},
          {placement_object_kind_t::instance_spot, "instance", maintenance_policy_kind_t::recreate,
           false, 20}};
        publish ();
    }

    void publish ()
    {
        EXPECT_EQ (repository->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
    }

    object_reserve_request_t request (std::string id, bool spot = false)
    {
        object_reserve_request_t r;
        r.key = {spot ? placement_object_kind_t::user_spot : placement_object_kind_t::actor, id};
        r.intent.stable_type = spot ? "room" : "player";
        r.target = {descriptor.mesh_name, node_rid_t::from_string (descriptor.rid.to_string ()), 1,
                    owner};
        r.capacity_bundle =
          spot ? placement_capacity_bundle_t{0, 1,
                                             spot_type_capacity_delta_t{
                                               placement_object_kind_t::user_spot, "room", 1}}
               : placement_capacity_bundle_t{1, 0, std::nullopt};
        return r;
    }

    authority_snapshot_t
    create (std::string id, const authority_snapshot_t *spot = nullptr, bool user_spot = false)
    {
        auto r = request (id, user_spot);
        const auto reserved =
          std::get<object_reserved_t> (repository->reserve (r).result ().value ());
        std::vector<std::byte> payload;
        if (user_spot)
            payload = encode_user_spot_authority_payload (
              {.stable_type = "room",
               .spot_id = id,
               .owner_id = owner.owner_id,
               .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
               .mesh_name = descriptor.mesh_name,
               .node_rid = r.target.node_rid,
               .node_generation = 1});
        else
            payload = encode_actor_authority_payload (
              {.stable_type = "player",
               .actor_id = id,
               .current_spot_id = spot ? "room" : "discard-entry",
               .current_spot_generation = spot ? spot->object_generation : 1,
               .current_spot_kind =
                 spot ? actor_authority_spot_kind_t::user : actor_authority_spot_kind_t::entry,
               .owner_id = owner.owner_id,
               .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
               .mesh_name = descriptor.mesh_name,
               .node_rid = r.target.node_rid,
               .node_generation = 1});
        return std::get<object_committed_t> (
                 repository->commit ({r.key, reserved.fence, payload}).result ().value ())
          .ready;
    }

    location_owner_token_t replace_owner ()
    {
        EXPECT_TRUE (std::holds_alternative<owner_lease_released_t> (
          repository->release_owner_lease (owner).result ().value ()));
        auto next = std::get<owner_lease_claimed_t> (
                      repository->claim_owner_lease (owner.owner_id, 1h).result ().value ())
                      .token;
        descriptor.lease_generation = next.lease_generation;
        ++descriptor.descriptor_revision;
        publish ();
        return next;
    }

    authority_snapshot_t read (authority_key_t key)
    {
        return std::get<authority_snapshot_t> (
          repository->read_authority_core (key).result ().value ());
    }
};
} // namespace
