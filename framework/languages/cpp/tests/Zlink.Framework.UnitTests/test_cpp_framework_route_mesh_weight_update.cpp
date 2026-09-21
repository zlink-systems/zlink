/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/foundation/operation_registry.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"

#include <gtest/gtest.h>

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;
namespace foundation = zlink::framework::runtime::foundation;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;

template <typename T> T await_task (zlink::framework::task_t<T> task)
{
    return std::move (task).result ().value ();
}

std::vector<std::uint8_t> bytes (const std::string &value)
{
    return {value.begin (), value.end ()};
}

bool pump_until (mesh::raw_mesh_node_owner_t &source,
                 mesh::raw_mesh_node_owner_t &target,
                 const std::function<bool ()> &condition)
{
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!condition () && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) await_task (source.drain_monitor_events (now));
        (void) await_task (target.drain_monitor_events (now));
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        std::this_thread::yield ();
    }
    return condition ();
}

std::optional<int> peer_channel_weight (mesh::raw_mesh_node_owner_t &source,
                                        const std::vector<std::uint8_t> &target_routing_id,
                                        const std::string &channel_name)
{
    const auto peer = source.topology ().peer (target_routing_id);
    if (!peer)
        return std::nullopt;
    for (const auto &channel : peer->descriptor.channels) {
        if (channel.name == channel_name)
            return channel.weight;
    }
    return std::nullopt;
}

void expect_successful_channel_request (mesh::raw_mesh_node_owner_t &source,
                                        mesh::raw_mesh_node_owner_t &target)
{
    using completion_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    std::promise<completion_t> promise;
    auto completion = promise.get_future ();
    const protocol::application_payload_t request{"WeightRequest", "application/json",
                                                  bytes ("request")};
    ASSERT_TRUE (await_task (
      source.request_to_channel ("X", request, 2s,
                                 [&promise] (foundation::operation_terminal_t terminal,
                                             std::vector<std::uint8_t> payload) mutable {
                                     promise.set_value ({terminal, std::move (payload)});
                                 })));

    std::optional<mesh::service_mailbox_claim_t> claim;
    const auto request_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < request_deadline) {
        claim =
          target.mailbox ().try_claim (mesh::service_mailbox_domain_t::application, 1, 64u * 1024u);
        if (!claim) {
            ASSERT_NE (
              mesh::raw_mesh_pump_result_t::protocol_error,
              await_task (target.pump_one (mesh::service_liveness_registry_t::clock_t::now ())));
        }
    }
    ASSERT_TRUE (claim);
    ASSERT_EQ (1u, claim->records.size ());
    const protocol::application_payload_t reply{"WeightReply", "application/json", bytes ("reply")};
    ASSERT_TRUE (target.reply (claim->records.front (), reply));
    ASSERT_TRUE (target.mailbox ().release (*claim));

    const auto reply_deadline = std::chrono::steady_clock::now () + 2s;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < reply_deadline) {
        ASSERT_NE (
          mesh::raw_mesh_pump_result_t::protocol_error,
          await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now (), false)));
    }
    ASSERT_EQ (std::future_status::ready, completion.wait_for (0ms));
    EXPECT_EQ (foundation::operation_terminal_t::completed, completion.get ().first);
}

TEST (CppFrameworkRouteMeshWeightUpdate, SoleTargetStopsAndResumesAfterLiveWeightChanges)
{
    auto context = std::make_shared<zlink::context_t> ();
    auto target_state =
      std::make_shared<zlink::framework::detail::mesh_node_builder_state_t> ("weight-update-mesh");
    target_state->core_context = context;
    target_state->listen_endpoint = "inproc://weight-update-target";
    target_state->routing_id = zlink::routing_id_t::from ("weight-update-target");
    target_state->channels.emplace (
      "X", zlink::framework::detail::mesh_channel_registration_t{100, {}, true, true});
    zlink::framework::detail::mesh_node_runtime_t target_node (target_state);
    target_node.start ();
    auto &target = target_node.native_node ().transport ();

    mesh::service_node_descriptor_t source_descriptor;
    source_descriptor.mesh_name = "weight-update-mesh";
    source_descriptor.node_routing_id = bytes ("weight-update-source");
    source_descriptor.lifecycle_generation = 1;
    source_descriptor.descriptor_revision = 1;
    source_descriptor.advertised_endpoint = "inproc://weight-update-source";
    mesh::raw_mesh_node_owner_t source ({std::move (source_descriptor)}, context);
    source.start ();

    const auto target_descriptor = target.topology ().local_descriptor ();
    source.expect_peer (target_descriptor);
    ASSERT_TRUE (source.connect_peer (target.endpoint (), target_descriptor));
    ASSERT_TRUE (pump_until (source, target, [&] {
        return source.topology ().peer (target_descriptor.node_routing_id).has_value ();
    }));

    expect_successful_channel_request (source, target);

    target_node.set_channel_weight ("X", 0);
    ASSERT_TRUE (pump_until (
      source, target,
      [&] { return peer_channel_weight (source, target_descriptor.node_routing_id, "X") == 0; }))
      << "peer did not receive the runtime weight-zero descriptor update";
    const protocol::application_payload_t rejected{"WeightRequest", "application/json",
                                                   bytes ("request")};
    EXPECT_FALSE (await_task (source.request_to_channel ("X", rejected, 2s, [] (auto, auto) {})));

    target_node.set_channel_weight ("X", 100);
    ASSERT_TRUE (pump_until (
      source, target,
      [&] { return peer_channel_weight (source, target_descriptor.node_routing_id, "X") == 100; }))
      << "peer did not receive the restored positive-weight descriptor update";
    expect_successful_channel_request (source, target);

    source.close ();
    target_node.stop ();
}

} // namespace
