/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_bound_session_fixture.hpp"
#include <cassert>
#include <utility>

namespace zlink::framework::test::m6a
{
using namespace std::chrono_literals;

protocol::bound_session_bind_t
bound_session_bind_request (const mesh::service_node_descriptor_t &target, std::string actor_id)
{
    return protocol::bound_session_bind_t{
      0,
      {std::move (actor_id), 1, target.node_routing_id, target.lifecycle_generation, 1, 1},
      bytes ("session-owner"),
      {protocol::bound_session_binding_state_t::active, 1}};
}

void complete_bound_session_bind (
  mesh::raw_mesh_node_owner_t &source,
  mesh::raw_mesh_node_owner_t &target,
  std::future<std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>> &completion,
  std::string_view expected_actor)
{
    std::shared_ptr<mesh::service_mailbox_claim_t> claim;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
        claim =
          target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        if (!claim) {
            const auto pumped =
              await_task (target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
            assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        }
    }
    assert (claim && claim->records.size () == 1);
    const auto &record = (*claim->records.front ());
    const auto decoded =
      protocol::decode_bound_session_bind (protocol::message_bytes (record.parts ().front ()));
    assert (decoded.actor.actor_id == expected_actor);
    assert (record.correlation && decoded.correlation == *record.correlation);
    const auto accepted =
      decoded.binding.state == protocol::bound_session_binding_state_t::active
        ? std::make_optional (protocol::stream_bind_reply_tail_t{
            decoded.binding.generation, decoded.actor.authority_owner_generation})
        : std::nullopt;
    assert (target.reply_bound_session_bind (record, 0, 0, accepted));
    assert (target.mailbox ().release (*claim));

    const auto completion_deadline = std::chrono::steady_clock::now () + 2s;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < completion_deadline) {
        const auto pumped =
          await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (completion.wait_for (0ms) == std::future_status::ready);
    const auto settled = completion.get ();
    assert (settled.first == foundation::operation_terminal_t::completed);
    const auto reply = protocol::decode_reply_header (settled.second);
    assert (reply.terminal_result == 0 && reply.failure_code == 0);
    if (accepted)
        assert (protocol::decode_stream_bind_reply (settled.second).binding == accepted);
}
}
