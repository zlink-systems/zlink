/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_advertised_receive_chunk_limit_wiring (test_context_t &test)
{
    namespace spots = zlink::framework::detail;

    /* Target advertises a fixed 32768-byte inbound cap on every actor
     * join reply. */
    test.require (spots::spot_actor_join_advertised_receive_chunk_limit_bytes == 32768,
                  "join reply must advertise a 32768-byte receive chunk limit");

    /* Round-trip: to_json/from_json preserve receiveChunkLimitBytes. */
    {
        spots::spot_actor_join_route_reply_t reply;
        reply.result_code = 0;
        reply.actor_node_rid = "node-a";
        reply.actor_type = "demo.actor";
        reply.actor_id = "actor-1";
        reply.actor_generation = 7;
        reply.payload = {1, 2, 3};
        reply.receive_chunk_limit_bytes =
          spots::spot_actor_join_advertised_receive_chunk_limit_bytes;

        nlohmann::json encoded = reply;
        const auto decoded = encoded.get<spots::spot_actor_join_route_reply_t> ();
        test.require (decoded.receive_chunk_limit_bytes == 32768,
                      "join reply round-trip must preserve receiveChunkLimitBytes");
        test.require (decoded.actor_id == "actor-1" && decoded.actor_generation == 7,
                      "join reply round-trip must preserve unrelated fields");
    }

    /* Tolerant decode: a peer on an older schema omits the field
     * entirely; absence must decode to 0, not throw. */
    {
        nlohmann::json legacy{{"resultCode", 0},           {"actorNodeRid", "node-a"},
                              {"actorType", "demo.actor"}, {"actorId", "actor-1"},
                              {"actorGeneration", 7},      {"payload", ""}};
        const auto decoded = legacy.get<spots::spot_actor_join_route_reply_t> ();
        test.require (
          decoded.receive_chunk_limit_bytes == 0,
          "legacy join reply missing receiveChunkLimitBytes must decode as 0 (not advertised)");
    }

    /* Source-side consumption: min(configured, advertised>0?advertised:local)
     * applied to the direct-transfer chunk-plan cap. */
    test.require (maintenance_runtime_t::apply_advertised_receive_chunk_limit (262144, 0) == 262144,
                  "advertised=0 (not advertised) must leave the local chunk cap unchanged");
    test.require (maintenance_runtime_t::apply_advertised_receive_chunk_limit (262144, 32768)
                    == 32768,
                  "a smaller advertised cap must win over a larger local budget");
    test.require (maintenance_runtime_t::apply_advertised_receive_chunk_limit (16384, 32768)
                    == 16384,
                  "a smaller local budget must still win over a larger advertised cap");

    /* Chunk-split proof: the same payload plans into more chunks once
     * capped by a smaller advertised limit. */
    const std::vector<std::uint8_t> payload (20000, 0x5a);
    const auto uncapped_limit =
      maintenance_runtime_t::apply_advertised_receive_chunk_limit (262144, 0);
    const auto capped_limit =
      maintenance_runtime_t::apply_advertised_receive_chunk_limit (262144, 8192);
    const auto chunk_count = [] (std::size_t total, std::uint64_t limit) {
        return limit == 0 ? std::size_t{0} : (total + limit - 1) / limit;
    };
    test.require (chunk_count (payload.size (), uncapped_limit) == 1
                    && chunk_count (payload.size (), capped_limit) == 3,
                  "an advertised cap smaller than the local budget must split the same payload "
                  "into more chunks");
}

// C-5 increment 2b (originate opt-in reverted b/49b6c-follow-up): the
// actorJoin(28) originate fence-gate (mesh_node_runtime_t::
// observe_spot_authority / admit_remote_application_actor_join_via_wire's
// caller in join_application_actor_to_spot) is a dormant mechanism. C++ does
// NOT originate a live cross-node actorJoin(28) per spec 51 §9 ("C++ and .NET
// ... do not originate a cross-node actorJoin operation"); the production
// join_application_actor_to_spot remote branch does NOT call
// observe_spot_authority, so observed_spot_authority always returns nullopt
// there and the branch falls through to the JSON admission path. Activating
// the opt-in (1b3b21b2e3, reverted) took an incomplete canonical receiver and
// broke ST-B1; completing that receiver is H-12/H-15/H-4a. This test still
// proves the gate MECHANISM is real (not dead code) "for whichever future
// caller opts a peer in": it directly calls observe_spot_authority and reads
// the observation back. It does not exercise
// join_application_actor_to_spot directly (the full remote-join call chain
// fails deep inside completion delivery for a synthetic, never-locally-
// created Actor regardless of which path is taken, and so cannot
// discriminate between them); it proves the gate mechanism itself directly
// -- a freshly started node's cache has no entry for a Spot address
// nothing has observed, so the exact lookup join_application_actor_to_spot
// performs returns nullopt, and after an explicit observe_spot_authority
// call with a fully valid, nonzero fence (mirroring what the production
// call site now does with a resolved spot_address_t), that same lookup
// returns exactly what was recorded -- this is the one piece of state
// the gate actually branches on.
void test_actor_join_wire_gate_records_target_authority (test_context_t &test)
{
    namespace detail = zlink::framework::detail;

    const auto core_context = std::make_shared<zlink::context_t> ();
    auto state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("actor-join-gate-mesh");
    state->core_context = core_context;
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("gate-node");
    detail::mesh_node_runtime_t node (state);
    node.start ().result ().value ();

    const auto target_rid = zlink::routing_id_t::from ("gate-target");
    test.require (!node.observed_spot_authority (target_rid, "gate-spot", 5).has_value (),
                  "a freshly started node's cache has no entry for a Spot address "
                  "nothing has observed or joined yet");

    node.observe_spot_authority (target_rid, "gate-spot", 5, 9, 7, 8);
    const auto observed = node.observed_spot_authority (target_rid, "gate-spot", 5);
    test.require (observed && observed->target_node_generation == 9
                    && observed->authority_owner_generation == 7
                    && observed->owner_lease_generation == 8,
                  "an explicit observe_spot_authority call must be readable back "
                  "exactly -- proving the gate mechanism itself is real, not dead code, "
                  "for whichever future caller opts a peer in");

    // A stale/mismatched key (wrong object generation) must not match --
    // the cache key is the full (node, Spot, generation) identity.
    test.require (!node.observed_spot_authority (target_rid, "gate-spot", 6).has_value (),
                  "a different object generation must not read back an unrelated "
                  "observation");

    node.stop ();
}

} // namespace m6c_test
