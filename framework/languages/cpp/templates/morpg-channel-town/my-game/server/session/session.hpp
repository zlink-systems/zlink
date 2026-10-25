#pragma once
#include "server/shared/config.hpp"
#include <zlink/framework.hpp>

namespace MyGame
{
namespace fx = zlink::framework;
class Session final : public fx::packet_stream_session_t
{
    fx::actor_manager_t &actors_;
    fx::serializer_registry_t &serializers_;

    static bool is_client_packet (const std::string &packet_name)
    {
        return packet_name == JoinTownRequest::packet_name
               || packet_name == ChangeChannelRequest::packet_name
               || packet_name == CreatePartyRequest::packet_name
               || packet_name == JoinPartyRequest::packet_name
               || packet_name == EnterDungeonRequest::packet_name
               || packet_name == AdvanceRequest::packet_name
               || packet_name == ViewRequest::packet_name
               || packet_name == SessionInfoRequest::packet_name;
    }

  public:
    Session (fx::actor_manager_t &actors, fx::serializer_registry_t &serializers) :
        actors_ (actors), serializers_ (serializers)
    {
    }
    fx::task_t<void> on_connected (fx::stream_t &) override { co_return; }
    fx::task_t<void> on_disconnected (fx::stream_t &) override { co_return; }
    fx::task_t<void> on_error (fx::stream_t &, const fx::stream_error_t &error) override
    {
        throw std::runtime_error (std::string (error.message ()));
        co_return;
    }
    fx::task_t<void> on_packet (fx::stream_t &stream,
                                const fx::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        if (!is_client_packet (dispatch.packet_name)) {
            const auto message = ActionRejected{Rejection::packet_not_allowed};
            co_await stream.write_packet (zlink::message_t::from_json (message))
              .packet_name (ActionRejected::packet_name)
              .async ();
            co_return;
        }
        auto bound = stream.actors ().bound ();
        if (bound.empty ()) {
            if (dispatch.packet_name != JoinTownRequest::packet_name)
                throw std::invalid_argument ("JoinTownRequest must be first");
            const auto request = serializers_.get<JoinTownRequest> ().deserialize (
              fx::encoded_payload_t::from_bytes (payload.bytes ()));
            const auto ensured =
              co_await actors_.get_or_create (fx::actor_id_t (request.playerId), Names::player)
                .in_mesh (Names::mesh)
                .async ();
            const auto ref = std::visit (
              [] (const auto &result) -> fx::actor_ref_t {
                  if constexpr (requires { result.actor; })
                      return result.actor;
                  else
                      throw std::runtime_error ("Actor creation rejected");
              },
              ensured);
            co_await stream.actors ().bind_or_get (ref).async ();
            bound = stream.actors ().bound ();
        }
        if (bound.size () != 1)
            throw std::runtime_error ("Exactly one Actor must be bound");
        auto &actor = bound.front ();
        if (dispatch.packet_name == SessionInfoRequest::packet_name) {
            co_await actor.bound_session ()
              .send (SessionInfo{stream.session_id (), std::string (actor.actor_id ()),
                                 actor.ref ().object_generation ()})
              .async ();
            co_return;
        }
        co_await actor.relay (std::string (dispatch.packet_name), payload);
    }
};
}
