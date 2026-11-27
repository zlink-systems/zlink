#pragma once
#include "domain.hpp"
#include "player.hpp"
#include "server/shared/config.hpp"
#include <zlink/framework.hpp>

namespace MyGame
{
namespace fx = zlink::framework;
class Table final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    fx::actor_client_t &actors_;
    fx::route_client_t &routes_;
    const NodeConfig &config_;
    template <class T> fx::task_t<void> push (const std::string &id, const T &message)
    {
        co_await actors_.send (fx::actor_id_t (id), message).async ();
    }
    template <class T>
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const T &message)
    {
        if constexpr (std::is_same_v<T, HandResult>) {
            const auto seat = std::ranges::find (message.seats, actor.id (), &SeatView::playerId);
            if (seat != message.seats.end ())
                actor.state.chips = seat->chips;
        }
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
        const auto id = actor.id ();
        co_await actor.context ().bound_session ().send (view (id)).async ();
        const auto index = state.seat_of (id);
        if (index != Rules::no_seat)
            co_await actor.context ()
              .bound_session ()
              .send (HandDealt{state.handNumber, state.seats[index].cards})
              .async ();
    }

  public:
    TableState state;
    Table (fx::spot_context_t context,
           fx::actor_client_t &actors,
           fx::route_client_t &routes,
           const NodeConfig &config) :
        context_ (std::move (context)), actors_ (actors), routes_ (routes), config_ (config)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    template <auto Method, class Message = PlayerPacket<Method>>
    fx::task_t<void> forward (Player &actor, fx::message_context_t &context, const Message &message)
    {
        co_await (actor.*Method) (context, message);
        if constexpr (std::is_same_v<Message, FindMatch>) {
            if (actor.state.found)
                co_await actor.context ().bound_session ().send (view (actor.id ())).async ();
        }
    }
    void configure () override
    {
        context_.handlers ()
          .add_handler<&Table::commit> ()
          .add_handler<&Table::withdraw> ()
          .add_actor_send<&Table::committed> ()
          .add_actor_send<&Table::reconnect> ()
          .add_actor_send<&Table::inspect> ()
          .add_actor_send<&Table::start> ()
          .add_actor_send<&Table::bet> ()
          .add_actor_send<&Table::deliver<TableView>> ()
          .add_actor_send<&Table::deliver<HandDealt>> ()
          .add_actor_send<&Table::deliver<HandResult>> ();
        register_matching<Table> (context_.handlers ());
    }
    fx::task_t<fx::spot_create_response_t> on_create (const fx::message_t &message) override
    {
        if (message.empty ())
            co_return fx::spot_create_response_t::accept ();
        const auto request = message.decode<PrepareRoom> ();
        if (request.assignmentId != context_.spot_id ()
            || !request.assignmentId.starts_with (MatchNames::room_prefix)
            || request.members.size () != Rules::minimum_players
            || request.members[0] == request.members[1])
            co_return fx::spot_create_response_t::reject ();
        state.matchMembers = request.members;
        state.roomPhase = RoomPhase::prepared;
        co_return fx::spot_create_response_t::accept ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &message) override
    {
        const auto request = message.decode<TableAdmission> ();
        if (request.playerId != id || request.tableId != context_.spot_id ()
            || !Rules::is_contract_integer (request.chips))
            throw std::invalid_argument ("Table join identity or balance mismatch");
        if (state.roomPhase == RoomPhase::withdrawn
            || (state.roomPhase != RoomPhase::fixed
                && std::ranges::find (state.matchMembers, id) == state.matchMembers.end ()))
            co_return fx::spot_actor_join_result_t::reject ();
        co_return state.admit (id, request.chips) ? fx::spot_actor_join_result_t::accept ()
                                                  : fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &) override
    {
        if (state.roomPhase == RoomPhase::fixed)
            co_await publish ();
    }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
    fx::task_t<void> on_disconnect_actor (Player &actor) override { co_await actor.disconnect (); }
    fx::task_t<void> commit (const CommitRoom &request)
    {
        if (request.assignmentId != context_.spot_id () || state.roomPhase != RoomPhase::prepared)
            co_return;
        state.roomPhase = RoomPhase::committed;
        for (const auto &seat : state.seats)
            co_await push (seat.playerId, request);
    }
    void withdraw (const WithdrawRoom &request)
    {
        if (request.assignmentId == context_.spot_id () && state.roomPhase == RoomPhase::prepared) {
            state.roomPhase = RoomPhase::withdrawn;
            state.seats.clear ();
        }
    }
    fx::task_t<void> committed (Player &actor, fx::message_context_t &, const CommitRoom &request)
    {
        const auto id = actor.id ();
        const auto seat = state.seat_of (id);
        if (request.assignmentId != context_.spot_id () || seat == Rules::no_seat
            || actor.state.assignmentId != request.assignmentId)
            co_return;
        co_await actor.found (request.assignmentId, seat);
        co_await actor.context ().bound_session ().send (view (id)).async ();
    }
    fx::task_t<void>
    reconnect (Player &actor, fx::message_context_t &, const JoinTableRequest &request)
    {
        if (request.playerId != actor.id () || request.tableId != context_.spot_id ())
            throw std::invalid_argument ("Reconnect identity mismatch");
        co_await respond (actor);
    }
    fx::task_t<void> inspect (Player &actor, fx::message_context_t &, const TableViewRequest &)
    {
        co_await respond (actor);
    }
    fx::task_t<void> start (Player &actor, fx::message_context_t &, const StartHandRequest &)
    {
        const auto rejection = state.start (actor.id ());
        if (!rejection.empty ()) {
            co_await actor.context ().bound_session ().send (ActionRejected{rejection, 0}).async ();
            co_return;
        }
        co_await publish ();
        for (const auto &seat : state.seats)
            co_await push (seat.playerId, HandDealt{state.handNumber, seat.cards});
    }
    fx::task_t<void> bet (Player &actor, fx::message_context_t &, const BetRequest &request)
    {
        const auto outcome = state.bet (actor.id (), request);
        if (!outcome.rejection.empty ()) {
            co_await actor.context ()
              .bound_session ()
              .send (ActionRejected{outcome.rejection, 0})
              .async ();
            co_return;
        }
        if (outcome.result) {
            CommitRound round{context_.spot_id (), state.handNumber, {}};
            for (const auto &seat : outcome.result->seats)
                round.results.push_back ({seat.playerId, seat.chips});
            (void) co_await routes_.request_to_channel (player_records_channel, std::move (round))
              .async<RoundCommitted> ();
            state.apply_settlement (*outcome.result);
        }
        co_await publish (outcome.result);
    }
};
class TableRelocation final : public fx::spot_relocation_adapter_t<Table>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Table &table, std::stop_token) override
    {
        const auto payload = fx::serializer_registry_t{}.get<TableState> ().serialize (table.state);
        const auto bytes = payload.bytes ();
        co_return std::vector<std::byte> (bytes.begin (), bytes.end ());
    }
    fx::task_t<void>
    restore (Table &table, std::vector<std::byte> payload, std::stop_token) override
    {
        table.state = fx::serializer_registry_t{}.get<TableState> ().deserialize (
          fx::encoded_payload_t::from_bytes (std::span<const std::byte> (payload)));
        co_return;
    }
};
}
