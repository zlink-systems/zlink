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
        auto bound = stream.actors ().bound ();
        if (dispatch.packet_name != JoinWorldRequest::packet_name
            && dispatch.packet_name != MoveRequest::packet_name
            && dispatch.packet_name != OpenDungeonRequest::packet_name
            && dispatch.packet_name != SessionInfoRequest::packet_name) {
            const ActionRejected rejection{"packet-not-allowed", 0};
            if (bound.empty ())
                co_await stream
                  .write_packet (zlink::message_t::from_json (rejection))
                  .async ();
            else
                co_await bound.front ().bound_session ().send (rejection).async ();
            co_return;
        }
        if (bound.empty ()) {
            if (dispatch.packet_name != JoinWorldRequest::packet_name)
                throw std::invalid_argument ("JoinWorldRequest must be first");
            const auto request = serializers_.get<JoinWorldRequest> ().deserialize (
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
            const auto generation = actor.ref ().object_generation ();
            if (generation > static_cast<std::uint64_t> (Limits::max_safe_integer)) {
                co_await actor.bound_session ().send (ActionRejected{"integer-out-of-range", 0})
                  .async ();
                co_return;
            }
            co_await actor.bound_session ()
              .send (SessionInfo{stream.session_id (), std::string (actor.actor_id ()),
                                 static_cast<std::int64_t> (generation)})
              .async ();
            co_return;
        }
        co_await actor.relay (std::string (dispatch.packet_name), payload);
    }
};
}
