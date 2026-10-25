#pragma once
#include "player.hpp"
#include <set>
namespace MyGame
{
struct ZoneSnapshot
{
    std::vector<std::string> playerIds;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ZoneSnapshot, playerIds)
};

class Zone final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    const NodeConfig &config_;
    fx::actor_client_t &actors_;
    std::set<std::string> player_ids_;
    PlayerView view (const Player &actor) const
    {
        return {config_.WorldId, Names::channel_of (context_.spot_id ()), config_.Id,
                actor.gold.value ()};
    }
    fx::task_t<void> show (Player &actor)
    {
        co_await actor.context ().bound_session ().send (view (actor)).async ();
    }
    fx::task_t<void> publish_players ()
    {
        const ChannelView view{Names::channel_of (context_.spot_id ()),
                               {player_ids_.begin (), player_ids_.end ()}};
        for (const auto &player_id : player_ids_)
            co_await actors_.send (fx::actor_id_t (player_id), view).async ();
    }
    fx::task_t<void> remove_player (Player &actor)
    {
        if (player_ids_.erase (std::string (actor.context ().actor_id ().value ())) != 0)
            co_await publish_players ();
    }

  public:
    Zone (fx::spot_context_t context, NodeConfig &config, fx::actor_client_t &actors) :
        context_ (std::move (context)), config_ (config), actors_ (actors)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Zone::enter> ()
          .add_actor_send<&Zone::change> ()
          .add_actor_send<&Zone::earn> ()
          .add_actor_send<&Zone::read> ()
          .add_actor_send<&Zone::read_players> ()
          .add_actor_send<&Zone::deliver> ()
          .add_actor_send<&Zone::deliver_channel_view> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        co_return payload.decode<EnterWorldRequest> ().playerId == id
          ? fx::spot_actor_join_result_t::accept ()
          : fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &actor) override
    {
        player_ids_.insert (std::string (actor.context ().actor_id ().value ()));
        co_await actors_.send (actor.context ().actor_id (), view (actor)).async ();
        co_await publish_players ();
    }
    fx::task_t<void> on_leave_actor (Player &actor) override
    {
        co_await remove_player (actor);
    }
    fx::task_t<void> on_disconnect_actor (Player &actor) override
    {
        co_await remove_player (actor);
    }
    fx::task_t<void> read_players (Player &actor, fx::message_context_t &,
                                   const ChannelViewRequest &)
    {
        co_await actor.context ().bound_session ()
          .send (ChannelView{Names::channel_of (context_.spot_id ()),
                             std::vector<std::string> (player_ids_.begin (), player_ids_.end ())})
          .async ();
    }
    ZoneSnapshot capture () const
    {
        return {{player_ids_.begin (), player_ids_.end ()}};
    }
    void restore (const ZoneSnapshot &snapshot)
    {
        player_ids_ = std::set<std::string> (snapshot.playerIds.begin (), snapshot.playerIds.end ());
    }
    fx::task_t<void>
    enter (Player &actor, fx::message_context_t &, const EnterWorldRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity mismatch");
        co_await show (actor);
    }
    fx::task_t<void>
    change (Player &actor, fx::message_context_t &, const ChangeChannelRequest &request)
    {
        if (request.channel > GameRules::max_integer) {
            co_await actor.context ()
              .bound_session ()
              .send (ActionRejected{Rejection::integer_out_of_range, 0})
              .async ();
            co_return;
        }
        actor.context ()
          .join_spot (Names::zone_id (request.channel),
                      EnterWorldRequest{std::string (actor.context ().actor_id ().value ())})
          .defer ();
        co_return;
    }
    fx::task_t<void> earn (Player &actor, fx::message_context_t &, const EarnRequest &)
    {
        const auto decision = co_await actor.earn (context_);
        if (decision.accepted ())
            co_await show (actor);
        else
            co_await actor.context ().bound_session ()
              .send (ActionRejected{decision.reason, decision.retryAfterMs})
              .async ();
    }
    fx::task_t<void> read (Player &actor, fx::message_context_t &, const ViewRequest &)
    {
        co_await show (actor);
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const PlayerView &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
    fx::task_t<void>
    deliver_channel_view (Player &actor, fx::message_context_t &, const ChannelView &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
};
class ZoneRelocation final : public fx::spot_relocation_adapter_t<Zone>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Zone &spot, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (spot.capture ());
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void> restore (Zone &spot, std::vector<std::byte> payload, std::stop_token) override
    {
        spot.restore (zlink::message_t::from (std::span<const std::byte> (payload))
                        .parse_json<ZoneSnapshot> ());
        co_return;
    }
};
}
