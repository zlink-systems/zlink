#pragma once
#include "domain.hpp"
#include "server/shared/config.hpp"
#include <zlink/framework.hpp>

namespace MyGame
{
namespace fx = zlink::framework;
class Player final : public fx::actor_t
{
    fx::actor_context_t context_;

  public:
    explicit Player (fx::actor_context_t context) : context_ (std::move (context)) {}
    fx::actor_context_t &context () noexcept override { return context_; }
    const fx::actor_context_t &context () const noexcept override { return context_; }
    fx::task_t<void> on_join_completed (const fx::actor_join_completion_t &completion) override
    {
        if (std::holds_alternative<fx::actor_join_rejected_t> (completion))
            co_await context_.bound_session ().send (ActionRejected{Rules::table_full}).async ();
        else if (!std::holds_alternative<fx::actor_join_accepted_t> (completion))
            throw std::runtime_error ("Table join failed");
    }
};
class PlayerFactory final : public fx::actor_factory_t<Player>
{
  public:
    fx::task_t<std::shared_ptr<Player>> create (fx::actor_context_t context,
                                                std::stop_token) override
    {
        co_return std::make_shared<Player> (std::move (context));
    }
};
// Identity, membership and binding are owned by Framework.
class PlayerRelocation final : public fx::actor_relocation_adapter_t<Player>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Player &, std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }
    fx::task_t<void> restore (Player &, std::vector<std::byte>, std::stop_token) override
    {
        co_return;
    }
};
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
    void configure () override { context_.handlers ().add_actor_send<&Entry::join> (); }
    fx::task_t<void> join (Player &actor, fx::message_context_t &, const JoinTableRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity differs from Actor");
        const auto created = co_await spots_.get_or_create (request.tableId, Names::table)
                               .in_mesh (Names::mesh)
                               .async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Table creation rejected");
        actor.context ().join_spot (request.tableId, request).defer ();
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
};
class Table final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    fx::actor_client_t &actors_;
    const NodeConfig &config_;
    static std::string player_id (const Player &actor)
    {
        return std::string (actor.context ().actor_id ().value ());
    }
    template <class T> fx::task_t<void> push (const std::string &id, const T &message)
    {
        co_await actors_.send (fx::actor_id_t (id), message).async ();
    }
    template <class T>
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const T &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
    fx::task_t<void> publish (const std::optional<HandResult> &result = {})
    {
        for (const auto &seat : state.seats) {
            co_await push (seat.playerId, view (seat.playerId));
            if (result)
                co_await push (seat.playerId, *result);
        }
    }
    TableView view (const std::string &id) const
    {
        return state.view (id, context_.spot_id (), config_.Id);
    }
    fx::task_t<void> respond (Player &actor)
    {
        const auto id = player_id (actor);
        co_await actor.context ().bound_session ().send (view (id)).async ();
        const auto index = state.seat_of (id);
        if (state.inHand && index != Rules::no_seat && !state.seats[index].cards.empty ())
            co_await actor.context ()
              .bound_session ()
              .send (HandDealt{state.handNumber, state.seats[index].cards})
              .async ();
    }

  public:
    TableState state;
    Table (fx::spot_context_t context, fx::actor_client_t &actors, const NodeConfig &config) :
        context_ (std::move (context)), actors_ (actors), config_ (config)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Table::reconnect> ()
          .add_actor_send<&Table::inspect> ()
          .add_actor_send<&Table::start> ()
          .add_actor_send<&Table::bet> ()
          .add_actor_send<&Table::deliver<TableView>> ()
          .add_actor_send<&Table::deliver<HandDealt>> ()
          .add_actor_send<&Table::deliver<HandResult>> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &message) override
    {
        const auto request = message.decode<JoinTableRequest> ();
        if (request.playerId != id || request.tableId != context_.spot_id ())
            throw std::invalid_argument ("Table join identity mismatch");
        co_return state.admit (id) ? fx::spot_actor_join_result_t::accept ()
                                   : fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_await publish (); }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
    fx::task_t<void> on_disconnect_actor (Player &) override { co_return; }
    fx::task_t<void>
    reconnect (Player &actor, fx::message_context_t &, const JoinTableRequest &request)
    {
        if (request.playerId != player_id (actor) || request.tableId != context_.spot_id ())
            throw std::invalid_argument ("Reconnect identity mismatch");
        co_await respond (actor);
    }
    fx::task_t<void> inspect (Player &actor, fx::message_context_t &, const TableViewRequest &)
    {
        co_await respond (actor);
    }
    fx::task_t<void> start (Player &actor, fx::message_context_t &, const StartHandRequest &)
    {
        const auto rejection = state.start (player_id (actor));
        if (!rejection.empty ()) {
            co_await actor.context ().bound_session ().send (ActionRejected{rejection}).async ();
            co_return;
        }
        co_await publish ();
        for (const auto &seat : state.seats)
            co_await push (seat.playerId, HandDealt{state.handNumber, seat.cards});
    }
    fx::task_t<void> bet (Player &actor, fx::message_context_t &, const BetRequest &request)
    {
        const auto outcome = state.bet (player_id (actor), request);
        if (!outcome.rejection.empty ()) {
            co_await actor.context ()
              .bound_session ()
              .send (ActionRejected{outcome.rejection})
              .async ();
            co_return;
        }
        co_await publish (outcome.result);
    }
};
class TableRelocation final : public fx::spot_relocation_adapter_t<Table>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Table &table, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (table.state);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void>
    restore (Table &table, std::vector<std::byte> payload, std::stop_token) override
    {
        table.state =
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<TableState> ();
        co_return;
    }
};
}
