#pragma once
#include "player.hpp"
namespace MyGame
{
class Entry final : public fx::entry_spot_t<Player>
{
    fx::entry_spot_context_t context_;
    fx::spot_manager_t &spots_;
    fx::task_t<void> join (Player &actor, const std::string &party, bool create)
    {
        if (!nonblank (party) || (!create && !(co_await spots_.find(party)))) {
            co_await actor.context ()
              .bound_session ()
              .send (ActionRejected{Rejection::invalid_party})
              .async ();
            co_return;
        }
        const auto id = party;
        if (create) {
            const auto result = co_await spots_.get_or_create(id, Names::party).in_mesh(Names::mesh).async();
            if (result.state == fx::spot_create_state_t::rejected) throw std::runtime_error("Party creation rejected");
        }
        actor.context ().join_spot (id, PartyJoin{party, create}).defer ();
    }

  public:
    Entry (fx::entry_spot_context_t context, fx::spot_manager_t &spots) :
        context_ (std::move (context)), spots_ (spots)
    {
    }
    fx::entry_spot_context_t &context () noexcept override { return context_; }
    const fx::entry_spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Entry::login> ()
          .add_actor_send<&Entry::create> ()
          .add_actor_send<&Entry::enter> ();
    }
    fx::task_t<void> login (Player &actor, fx::message_context_t &, const JoinLobbyRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity mismatch");
        co_await actor.context ().bound_session ().send (LobbyView{}).async ();
    }
    fx::task_t<void>
    create (Player &actor, fx::message_context_t &, const CreatePartyRequest &)
    {
        co_await join (actor, new_id("party-"), true);
    }
    fx::task_t<void> enter (Player &actor, fx::message_context_t &, const JoinPartyRequest &request)
    {
        co_await join (actor, request.partyId, false);
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
};
}
