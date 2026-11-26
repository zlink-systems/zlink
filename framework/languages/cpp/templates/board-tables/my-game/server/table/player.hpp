#pragma once
#include "server/shared/contracts.hpp"
#include "server/shared/identity.hpp"
#include "server/shared/match_contracts.hpp"
#include "server/shared/player_records.hpp"
#include <zlink/framework.hpp>
#include <algorithm>
#include <utility>

namespace MyGame
{
namespace fx = zlink::framework;

class PlayerServices final : public fx::hosted_service_t
{
  public:
    fx::route_client_t *routes = nullptr;

    fx::task_t<void> start (fx::service_provider_t &services) override
    {
        routes = &services.get_required<fx::route_client_t> ();
        co_return;
    }
    void stop () noexcept override {}
};

enum class PlayerJoin
{
    none,
    fixed_table,
    assigned_room,
    return_to_entry
};

struct PlayerState
{
    std::string requestKey;
    std::string ticketId;
    std::string assignmentId;
    std::string joiningAssignment;
    std::vector<std::string> withdrawn;
    std::optional<MatchFound> found;
    std::optional<std::int64_t> chips;
    PlayerJoin joining = PlayerJoin::none;
};

class Player final : public fx::actor_t
{
    fx::actor_context_t context_;
    PlayerServices &services_;

    fx::task_t<void> report (const EnterReport &report)
    {
        co_await services_.routes->send_to_spot (MatchNames::pool_key, report)
          .instance_spot (MatchNames::pool)
          .in_mesh (Names::mesh)
          .async ();
    }

    void clear_ticket ()
    {
        state.requestKey.clear ();
        state.ticketId.clear ();
        state.assignmentId.clear ();
        state.withdrawn.clear ();
    }

    void leave_room ()
    {
        if (state.joining == PlayerJoin::none) {
            context_.join_entry_spot ().defer ();
            state.joining = PlayerJoin::return_to_entry;
        }
    }

  public:
    PlayerState state;

    Player (fx::actor_context_t context, PlayerServices &services) :
        context_ (std::move (context)), services_ (services)
    {
    }
    fx::actor_context_t &context () noexcept override { return context_; }
    const fx::actor_context_t &context () const noexcept override { return context_; }
    std::string id () const { return std::string (context_.actor_id ().value ()); }

    fx::task_t<std::int64_t> load_chips ()
    {
        if (!state.chips) {
            const auto reply = co_await services_.routes
                                 ->request_to_channel (player_records_channel, LoadPlayer{id ()})
                                 .async<PlayerRecord> ();
            state.chips = reply.chips;
        }
        co_return *state.chips;
    }

    void join_table (const JoinTableRequest &request)
    {
        if (state.joining == PlayerJoin::none && state.ticketId.empty ()) {
            context_
              .join_spot (request.tableId, TableAdmission{id (), request.tableId, *state.chips})
              .defer ();
            state.joining = PlayerJoin::fixed_table;
        }
    }

    fx::task_t<void> on_join_completed (const fx::actor_join_completion_t &completion) override
    {
        const auto join = std::exchange (state.joining, PlayerJoin::none);
        const bool accepted = std::holds_alternative<fx::actor_join_accepted_t> (completion);
        if (join == PlayerJoin::assigned_room) {
            auto assignment = std::exchange (state.joiningAssignment, {});
            if (assignment == state.assignmentId && !state.ticketId.empty ()) {
                const EnterReport entered{state.ticketId, assignment, accepted};
                co_await report (entered);
            } else if (accepted) {
                leave_room ();
            }
        } else if (join == PlayerJoin::fixed_table
                   && std::holds_alternative<fx::actor_join_rejected_t> (completion)) {
            const ActionRejected rejection{Rules::table_full, 0};
            co_await context_.bound_session ().send (rejection).async ();
        } else if (join != PlayerJoin::none && !accepted) {
            throw std::runtime_error ("Player join failed");
        }
    }

    fx::task_t<void> find (fx::message_context_t &, const FindMatch &)
    {
        if (state.found) {
            co_await context_.bound_session ().send (*state.found).async ();
            co_return;
        }
        if (state.ticketId.empty ()) {
            co_await load_chips ();
            if (state.requestKey.empty ())
                state.requestKey = new_id ("request-");
            const MatchRequest request{state.requestKey, id ()};
            const auto queued =
              co_await services_.routes->request_to_spot (MatchNames::pool_key, request)
                .instance_spot (MatchNames::pool)
                .in_mesh (Names::mesh)
                .async<MatchQueued> ();
            state.ticketId = queued.ticketId;
        }
        const MatchQueued queued{state.ticketId};
        co_await context_.bound_session ().send (queued).async ();
    }

    fx::task_t<void> assigned (fx::message_context_t &, const MatchAssigned &message)
    {
        if (message.ticketId != state.ticketId || message.assignmentId.empty ()
            || message.assignmentId != message.roomId || !state.assignmentId.empty ()
            || std::ranges::find (state.withdrawn, message.assignmentId) != state.withdrawn.end ())
            co_return;
        state.assignmentId = message.assignmentId;
        if (state.joining != PlayerJoin::none) {
            const EnterReport failed{message.ticketId, message.assignmentId, false};
            co_await report (failed);
            co_return;
        }
        context_
          .join_spot (message.roomId, TableAdmission{id (), message.assignmentId, *state.chips})
          .defer ();
        state.joining = PlayerJoin::assigned_room;
        state.joiningAssignment = message.assignmentId;
    }

    fx::task_t<void> cancel (fx::message_context_t &context, const CancelMatch &message)
    {
        if (state.ticketId.empty () || message.ticketId != state.ticketId)
            co_return;
        const CancelTicket request{message.ticketId, MatchNames::cancelled};
        const auto reply =
          co_await services_.routes->request_to_spot (MatchNames::pool_key, request)
            .instance_spot (MatchNames::pool)
            .in_mesh (Names::mesh)
            .async<MatchCancelled> ();
        co_await cancelled (context, reply);
    }

    fx::task_t<void> cancelled (fx::message_context_t &, const MatchCancelled &message)
    {
        if (state.ticketId.empty () || message.ticketId != state.ticketId)
            co_return;
        const auto assignment = state.assignmentId;
        clear_ticket ();
        if (message.reason != MatchNames::disconnected)
            co_await context_.bound_session ().send (message).async ();
        if (!assignment.empty () && context_.spot_id () == assignment)
            leave_room ();
    }

    fx::task_t<void> withdrawn (fx::message_context_t &, const WithdrawRoom &message)
    {
        if (state.ticketId.empty ())
            co_return;
        if (std::ranges::find (state.withdrawn, message.assignmentId) == state.withdrawn.end ())
            state.withdrawn.push_back (message.assignmentId);
        if (message.assignmentId != state.assignmentId)
            co_return;
        state.assignmentId.clear ();
        if (context_.spot_id () == message.assignmentId)
            leave_room ();
    }

    fx::task_t<void> disconnect ()
    {
        if (!state.ticketId.empty ()) {
            const CancelTicket request{state.ticketId, MatchNames::disconnected};
            co_await services_.routes->send_to_spot (MatchNames::pool_key, request)
              .instance_spot (MatchNames::pool)
              .in_mesh (Names::mesh)
              .async ();
        }
    }

    fx::task_t<void> found (const std::string &room, int seat)
    {
        if (state.assignmentId != room || state.ticketId.empty ())
            co_return;
        state.found = MatchFound{state.ticketId, room, seat};
        clear_ticket ();
        co_await context_.bound_session ().send (*state.found).async ();
    }
};

class PlayerFactory final : public fx::actor_factory_t<Player>
{
    PlayerServices &services_;

  public:
    explicit PlayerFactory (PlayerServices &services) : services_ (services) {}
    fx::task_t<std::shared_ptr<Player>> create (fx::actor_context_t context,
                                                std::stop_token) override
    {
        co_return std::make_shared<Player> (std::move (context), services_);
    }
};

template <class T>
std::type_identity<T> player_packet (fx::task_t<void> (Player::*) (fx::message_context_t &,
                                                                   const T &));

template <auto Method> using PlayerPacket = typename decltype (player_packet (Method))::type;

template <class Spot> void register_matching (fx::spot_handler_registry_t handlers)
{
    handlers.template add_actor_send<&Spot::template forward<&Player::find>> ()
      .template add_actor_send<&Spot::template forward<&Player::assigned>> ()
      .template add_actor_send<&Spot::template forward<&Player::cancel>> ()
      .template add_actor_send<&Spot::template forward<&Player::cancelled>> ()
      .template add_actor_send<&Spot::template forward<&Player::withdrawn>> ();
}
}

namespace zlink::json
{
template <> struct enum_traits<::MyGame::PlayerJoin>
{
    using Join = ::MyGame::PlayerJoin;
    static constexpr auto values =
      std::array{enum_value (Join::none, "none"), enum_value (Join::fixed_table, "fixed-table"),
                 enum_value (Join::assigned_room, "assigned-room"),
                 enum_value (Join::return_to_entry, "return-to-entry")};
};

template <> struct dto_traits<::MyGame::PlayerState>
{
    static constexpr auto fields =
      std::tuple{field ("requestKey", &::MyGame::PlayerState::requestKey),
                 field ("ticketId", &::MyGame::PlayerState::ticketId),
                 field ("assignmentId", &::MyGame::PlayerState::assignmentId),
                 field ("joiningAssignment", &::MyGame::PlayerState::joiningAssignment),
                 field ("withdrawn", &::MyGame::PlayerState::withdrawn),
                 field ("found", &::MyGame::PlayerState::found),
                 field ("joining", &::MyGame::PlayerState::joining),
                 field ("chips", &::MyGame::PlayerState::chips, zlink::json::defaulted)};
};
}

namespace MyGame
{
class PlayerRelocation final : public fx::actor_relocation_adapter_t<Player>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Player &player, std::stop_token) override
    {
        const auto payload =
          fx::serializer_registry_t{}.get<PlayerState> ().serialize (player.state);
        const auto bytes = payload.bytes ();
        co_return std::vector<std::byte> (bytes.begin (), bytes.end ());
    }
    fx::task_t<void>
    restore (Player &player, std::vector<std::byte> state, std::stop_token) override
    {
        if (!state.empty ())
            player.state = fx::serializer_registry_t{}.get<PlayerState> ().deserialize (
              fx::encoded_payload_t::from_bytes (std::span<const std::byte> (state)));
        co_return;
    }
};
}
