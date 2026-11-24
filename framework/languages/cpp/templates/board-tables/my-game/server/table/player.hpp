#pragma once
#include "server/shared/contracts.hpp"
#include "server/shared/identity.hpp"
#include "server/shared/match_contracts.hpp"
#include <zlink/framework.hpp>
#include <algorithm>
#include <utility>

namespace MyGame
{
namespace fx = zlink::framework;

class PlayerServices final : public fx::hosted_service_t
{
  public:
    fx::spot_manager_t *spots = nullptr;
    fx::route_client_t *routes = nullptr;

    fx::task_t<void> start (fx::service_provider_t &services) override
    {
        spots = &services.get_required<fx::spot_manager_t> ();
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

struct PlayerMatch
{
    std::string requestKey;
    std::string ticketId;
    std::string assignmentId;
    std::string joiningAssignment;
    std::vector<std::string> withdrawn;
    std::optional<MatchFound> found;
    PlayerJoin joining = PlayerJoin::none;
};

class Player final : public fx::actor_t
{
    fx::actor_context_t context_;
    PlayerServices &services_;

    fx::task_t<void> report (const EnterReport &report)
    {
        co_await services_.routes->send_to_spot (MatchNames::pool_key, report).async ();
    }

    void clear_ticket ()
    {
        match.requestKey.clear ();
        match.ticketId.clear ();
        match.assignmentId.clear ();
        match.withdrawn.clear ();
    }

    void leave_room ()
    {
        if (match.joining == PlayerJoin::none) {
            context_.join_entry_spot ().defer ();
            match.joining = PlayerJoin::return_to_entry;
        }
    }

  public:
    PlayerMatch match;

    Player (fx::actor_context_t context, PlayerServices &services) :
        context_ (std::move (context)), services_ (services)
    {
    }
    fx::actor_context_t &context () noexcept override { return context_; }
    const fx::actor_context_t &context () const noexcept override { return context_; }
    std::string id () const { return std::string (context_.actor_id ().value ()); }

    void join_table (const JoinTableRequest &request)
    {
        if (match.joining == PlayerJoin::none && match.ticketId.empty ()) {
            context_.join_spot (request.tableId, request).defer ();
            match.joining = PlayerJoin::fixed_table;
        }
    }

    fx::task_t<void> on_join_completed (const fx::actor_join_completion_t &completion) override
    {
        const auto join = std::exchange (match.joining, PlayerJoin::none);
        const bool accepted = std::holds_alternative<fx::actor_join_accepted_t> (completion);
        if (join == PlayerJoin::assigned_room) {
            auto assignment = std::exchange (match.joiningAssignment, {});
            if (assignment == match.assignmentId && !match.ticketId.empty ()) {
                const EnterReport entered{match.ticketId, assignment, accepted};
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
        if (match.found) {
            co_await context_.bound_session ().send (*match.found).async ();
            co_return;
        }
        if (match.ticketId.empty ()) {
            const auto pool =
              co_await services_.spots->get_or_create (MatchNames::pool_key, MatchNames::pool)
                .in_mesh (Names::mesh)
                .async ();
            if (pool.state == fx::spot_create_state_t::rejected)
                throw std::runtime_error ("Pool creation rejected");
            if (match.requestKey.empty ())
                match.requestKey = new_id ("request-");
            const MatchRequest request{match.requestKey, id ()};
            const auto queued =
              co_await services_.routes->request_to_spot (MatchNames::pool_key, request)
                .async<MatchQueued> ();
            match.ticketId = queued.ticketId;
        }
        const MatchQueued queued{match.ticketId};
        co_await context_.bound_session ().send (queued).async ();
    }

    fx::task_t<void> assigned (fx::message_context_t &, const MatchAssigned &message)
    {
        if (message.ticketId != match.ticketId || message.assignmentId.empty ()
            || message.assignmentId != message.roomId || !match.assignmentId.empty ()
            || std::ranges::find (match.withdrawn, message.assignmentId) != match.withdrawn.end ())
            co_return;
        match.assignmentId = message.assignmentId;
        if (match.joining != PlayerJoin::none) {
            const EnterReport failed{message.ticketId, message.assignmentId, false};
            co_await report (failed);
            co_return;
        }
        context_.join_spot (message.roomId, message).defer ();
        match.joining = PlayerJoin::assigned_room;
        match.joiningAssignment = message.assignmentId;
    }

    fx::task_t<void> cancel (fx::message_context_t &context, const CancelMatch &message)
    {
        if (match.ticketId.empty () || message.ticketId != match.ticketId)
            co_return;
        const CancelTicket request{message.ticketId, MatchNames::cancelled};
        const auto reply =
          co_await services_.routes->request_to_spot (MatchNames::pool_key, request)
            .async<MatchCancelled> ();
        co_await cancelled (context, reply);
    }

    fx::task_t<void> cancelled (fx::message_context_t &, const MatchCancelled &message)
    {
        if (match.ticketId.empty () || message.ticketId != match.ticketId)
            co_return;
        const auto assignment = match.assignmentId;
        clear_ticket ();
        if (message.reason != MatchNames::disconnected)
            co_await context_.bound_session ().send (message).async ();
        if (!assignment.empty () && context_.spot_id () == assignment)
            leave_room ();
    }

    fx::task_t<void> withdrawn (fx::message_context_t &, const WithdrawRoom &message)
    {
        if (match.ticketId.empty ())
            co_return;
        if (std::ranges::find (match.withdrawn, message.assignmentId) == match.withdrawn.end ())
            match.withdrawn.push_back (message.assignmentId);
        if (message.assignmentId != match.assignmentId)
            co_return;
        match.assignmentId.clear ();
        if (context_.spot_id () == message.assignmentId)
            leave_room ();
    }

    fx::task_t<void> disconnect ()
    {
        if (!match.ticketId.empty ()) {
            const CancelTicket request{match.ticketId, MatchNames::disconnected};
            co_await services_.routes->send_to_spot (MatchNames::pool_key, request).async ();
        }
    }

    fx::task_t<void> found (const std::string &room, int seat)
    {
        if (match.assignmentId != room || match.ticketId.empty ())
            co_return;
        match.found = MatchFound{match.ticketId, room, seat};
        clear_ticket ();
        co_await context_.bound_session ().send (*match.found).async ();
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

template <> struct dto_traits<::MyGame::PlayerMatch>
{
    static constexpr auto fields =
      std::tuple{field ("requestKey", &::MyGame::PlayerMatch::requestKey),
                 field ("ticketId", &::MyGame::PlayerMatch::ticketId),
                 field ("assignmentId", &::MyGame::PlayerMatch::assignmentId),
                 field ("joiningAssignment", &::MyGame::PlayerMatch::joiningAssignment),
                 field ("withdrawn", &::MyGame::PlayerMatch::withdrawn),
                 field ("found", &::MyGame::PlayerMatch::found),
                 field ("joining", &::MyGame::PlayerMatch::joining)};
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
          fx::serializer_registry_t{}.get<PlayerMatch> ().serialize (player.match);
        const auto bytes = payload.bytes ();
        co_return std::vector<std::byte> (bytes.begin (), bytes.end ());
    }
    fx::task_t<void>
    restore (Player &player, std::vector<std::byte> state, std::stop_token) override
    {
        if (!state.empty ())
            player.match = fx::serializer_registry_t{}.get<PlayerMatch> ().deserialize (
              fx::encoded_payload_t::from_bytes (std::span<const std::byte> (state)));
        co_return;
    }
};
}
