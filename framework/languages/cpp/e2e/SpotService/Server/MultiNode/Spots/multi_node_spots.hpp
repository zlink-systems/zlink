/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/spot_service_contracts.hpp"
#include "../../Shared/scenario_state.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace e2e = zlink::framework::e2e::spot_service;

inline constexpr char multi_node_a_name[] = "multi-a";
inline constexpr char multi_node_b_name[] = "multi-b";

inline zlink::routing_id_t multi_node_target_rid (const std::string &value)
{
    return zlink::routing_id_t::from (value);
}

struct multi_node_actor_t : zlink::framework::actor_t
{
    explicit multi_node_actor_t (zlink::framework::actor_context_t value) :
        actor_id (value.actor_ref ().actor_id ().value ()),
        actor_ref (value.actor_ref ()),
        _actor_context (std::move (value))
    {
    }

    zlink::framework::actor_context_t &context () noexcept override
    { return _actor_context; }
    const zlink::framework::actor_context_t &context () const noexcept override
    { return _actor_context; }

    std::string actor_id;
    zlink::framework::actor_ref_t actor_ref;
    zlink::framework::actor_context_t _actor_context;
};

struct multi_node_actor_factory_t final
    : zlink::framework::actor_factory_t<multi_node_actor_t>
{
    zlink::framework::task_t<std::shared_ptr<multi_node_actor_t>>
    create (zlink::framework::actor_context_t context,
            std::stop_token) override
    {
        co_return std::make_shared<multi_node_actor_t> (
          std::move (context));
    }
};

template <const char *NodeName>
class multi_node_spot_t
    : public zlink::framework::spot_t<multi_node_actor_t>
{
  public:
    multi_node_spot_t (
      zlink::framework::spot_context_t context,
      scenario_state_t &state) :
        _state (state),
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override
    {
        return _context;
    }

    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<&multi_node_spot_t::state_request> ("StateReq");
        _context.handlers ().add_handler<&multi_node_spot_t::state_command> ("StateMsg");
        _context.handlers ().add_handler<&multi_node_spot_t::state_command> ("DirectSpotMsg");
        _context.handlers ().add_actor_request<&multi_node_spot_t::actor_probe> (
          "SpotOnlyActorProbeReq");
    }

    zlink::framework::task_t<void> on_initialize () override
    {
        _state.record ("MultiSpotInitialized", {}, _context.spot_id ());
        co_return;
    }

    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &request)
    {
        if (!request.empty ()) {
            std::optional<e2e::spot_only_mesh_req_t> command;
            try {
                command = request.decode<e2e::spot_only_mesh_req_t> ();
            }
            catch (const std::exception &) {
            }
            if (command) {
                const auto target_node =
                  std::string (NodeName) == multi_node_a_name ? multi_node_b_name
                                                              : multi_node_a_name;
                auto reply = co_await _context
                               .request_to<e2e::state_res_t> (
                                 multi_node_target_rid (target_node),
                                 multi_node_target_rid (command->target_spot_id),
                                 e2e::state_req_t{.op = "add", .amount = 7})
                               .timeout (std::chrono::milliseconds (3000))
                               .submit ();
                _context
                  .send_to (multi_node_target_rid (target_node),
                            multi_node_target_rid (command->target_spot_id),
                            e2e::direct_spot_msg_t{.source_actor_id = command->source_spot_id,
                                                   .value = "sm-f6-send-" + command->marker})
                  .submit ();
                _state.record ("SpotOnlyRequest", {}, _context.spot_id (),
                               "target=" + command->target_spot_id + "|value="
                                 + std::to_string (reply.value)
                                 + "|marker=" + command->marker);
            }
        }
        co_return zlink::framework::spot_create_response_t::accept ();
    }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view actor_id,
                   const zlink::framework::message_t &) override
    {
        _state.record ("SpotActorJoined", std::string (actor_id),
                       _context.spot_id ());
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }

    zlink::framework::task_t<void>
    on_actor_joined (multi_node_actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void>
    on_leave_actor (multi_node_actor_t &) override
    {
        co_return;
    }

    e2e::state_res_t state_request (const e2e::state_req_t &request)
    {
        if (request.op == "add") {
            _value += request.amount;
        }
        ++_sequence;
        _state.record ("MultiStateRequest", {}, _context.spot_id (),
                       std::to_string (_value));
        return {.spot_id = _context.spot_id (),
                .owner_node_rid = NodeName,
                .value = _value,
                .sequence = _sequence};
    }

    void state_command (const e2e::direct_spot_msg_t &request)
    {
        _state.record ("SpotStateCommand", request.source_actor_id,
                       _context.spot_id (), request.value);
    }

    e2e::actor_ping_res_t actor_probe (const multi_node_actor_t &actor,
                                       zlink::framework::spot_actor_request_context_t &,
                                       const e2e::actor_ping_req_t &request)
    {
        return {.actor_id = actor.actor_id,
                .node_rid = NodeName,
                .spot_id = _context.spot_id (),
                .value = request.value,
                .seen = 1};
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_context_t _context;
    int _value = 0;
    int _sequence = 0;
};

using multi_node_spot_a_t = multi_node_spot_t<multi_node_a_name>;
using multi_node_spot_b_t = multi_node_spot_t<multi_node_b_name>;

class multi_node_entry_spot_t
    : public zlink::framework::entry_spot_t<multi_node_actor_t>
{
  public:
    multi_node_entry_spot_t (
      zlink::framework::entry_spot_context_t context,
      scenario_state_t &state) :
        _state (state),
        _context (std::move (context))
    {
    }

    zlink::framework::entry_spot_context_t &context () noexcept override
    {
        return _context;
    }

    const zlink::framework::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_actor_request<&multi_node_entry_spot_t::join_spot_only> (
          "SpotOnlyJoinReq");
    }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (
      std::string_view,
      const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }

    zlink::framework::task_t<void>
    on_actor_joined (multi_node_actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void>
    on_leave_actor (multi_node_actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<e2e::spot_only_join_res_t>
    join_spot_only (multi_node_actor_t &actor,
                    zlink::framework::spot_actor_request_context_t &,
                    const e2e::spot_only_join_req_t &request)
    {
        if (request.actor_id != actor.actor_id) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::protocol_error,
              "spot-only join request actor does not match dispatched actor");
        }
        auto joined =
          co_await actor.context ()
            .join_spot ((request.target_spot_id),
                        zlink::framework::message_t {})
            .async ();
        const auto accepted = std::holds_alternative<zlink::framework::actor_join_accepted_t<zlink::framework::message_t>> (joined);
        _state.record ("SpotOnlyActorJoin", actor.actor_id, request.target_spot_id,
                       "accepted=" + std::string (accepted ? "true" : "false")
                         + "|marker=" + request.marker);
        co_return e2e::spot_only_join_res_t{.target_spot_id = request.target_spot_id,
                                            .actor_id = actor.actor_id,
                                            .accepted = accepted,
                                            .marker = request.marker};
    }

  private:
    scenario_state_t &_state;
    zlink::framework::entry_spot_context_t _context;
};
