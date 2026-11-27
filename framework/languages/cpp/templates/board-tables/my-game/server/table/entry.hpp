#pragma once
#include "player.hpp"

namespace MyGame
{
class Entry final : public fx::entry_spot_t<Player>
{
    fx::entry_spot_context_t context_;
    fx::spot_manager_t &spots_;

  public:
    Entry (fx::entry_spot_context_t context, fx::spot_manager_t &spots) :
        context_ (std::move (context)), spots_ (spots)
    {
    }
    fx::entry_spot_context_t &context () noexcept override { return context_; }
    const fx::entry_spot_context_t &context () const noexcept override { return context_; }
    template <auto Method, class Message = PlayerPacket<Method>>
    fx::task_t<void> forward (Player &actor, fx::message_context_t &context, const Message &message)
    {
        co_await (actor.*Method) (context, message);
    }
    void configure () override
    {
        context_.handlers ().add_actor_send<&Entry::join> ();
        register_matching<Entry> (context_.handlers ());
    }
    fx::task_t<void> join (Player &actor, fx::message_context_t &, const JoinTableRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity differs from Actor");
        if (request.reconnect) {
            co_await actor.context ()
              .bound_session ()
              .send (ActionRejected{Rules::not_seated, 0})
              .async ();
            co_return;
        }
        const auto created = co_await spots_.get_or_create (request.tableId, Names::table)
                               .in_mesh (Names::mesh)
                               .async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Table creation rejected");
        co_await actor.load_chips ();
        actor.join_table (request);
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
    fx::task_t<void> on_disconnect_actor (Player &actor) override { co_await actor.disconnect (); }
};
}
