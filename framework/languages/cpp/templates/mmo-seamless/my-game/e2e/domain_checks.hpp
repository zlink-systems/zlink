#pragma once
#include "server/zone/domain.hpp"
#include <iostream>
#include <limits>
#include <ranges>
#include <type_traits>

namespace MyGame::E2E
{
class DomainChecks
{
    int passed_ = 0;
    int assertions_ = 0;

    void require (bool condition, const std::string &message)
    {
        ++assertions_;
        if (!condition)
            throw std::runtime_error (message);
    }
    template <class Check> void check (const char *name, Check action)
    {
        action ();
        std::cout << "PASS " << name << std::endl;
        ++passed_;
    }
    ZonePositionDomain create (std::int64_t &now)
    {
        ZonePositionDomain domain ("zone-2", [&now] { return now; });
        domain.add_player ("player", EnterZone{Position{100}, 0, 0});
        return domain;
    }
    void require_rejected (ZonePositionDomain &domain,
                           double delta,
                           std::int64_t sequence,
                           const std::string &reason,
                           const std::string &player_id = "player")
    {
        const auto before = domain.state (player_id);
        const auto result = domain.move (player_id, MoveRequest{delta, sequence});
        require (result.rejection.has_value (), "Expected " + reason + " rejection");
        require (result.rejection->reason == reason,
                 "Expected " + reason + ", received " + result.rejection->reason);
        require (result.rejection->retryAfterMs == 0, reason + " must not report retry delay");
        require (domain.state (player_id) == before, "Rejected input changed domain state");
    }
    void rejects_non_finite_movement ()
    {
        std::int64_t now = 0;
        auto domain = create (now);
        for (const auto delta : {std::numeric_limits<double>::quiet_NaN (),
                                 std::numeric_limits<double>::infinity (),
                                 -std::numeric_limits<double>::infinity ()})
            require_rejected (domain, delta, 1, "non-finite");
    }
    void rejects_oversized_step ()
    {
        std::int64_t now = 0;
        auto domain = create (now);
        require_rejected (domain, ZonePositionDomain::MaxStep + 1, 1, "step-too-large");
    }
    void rejects_outside_world ()
    {
        ZonePositionDomain domain ("zone-6", [] { return 0; });
        domain.add_player (
          "edge", EnterZone{Position{Names::world_width - 1.0}, 0, 0});
        require_rejected (domain, 1, 1, "outside-world", "edge");
    }
    void rejects_until_server_retry ()
    {
        std::int64_t now = 0;
        auto domain = create (now);
        const auto first = domain.move ("player", MoveRequest{ZonePositionDomain::MaxStep, 1});
        require (first.accepted && first.position.x == 110,
                 "The initial move must have MaxStep budget");
        const auto before = domain.state ("player");
        const auto rejected = domain.move ("player", MoveRequest{ZonePositionDomain::MaxStep, 2});
        require (rejected.rejection.has_value (), "Expected move-budget rejection");
        const auto &reason = *rejected.rejection;
        require (reason.reason == "move-budget", "Expected move-budget rejection");
        require (reason.retryAfterMs > 0, "move-budget must report a positive retry delay");
        require (domain.state ("player") == before, "A rejected move changed position or sequence");
        now += reason.retryAfterMs;
        const auto accepted = domain.move ("player", MoveRequest{ZonePositionDomain::MaxStep, 2});
        require (accepted.accepted && accepted.position.x == 120,
                 "The server retry delay was insufficient");
    }
    void rejects_invalid_sequence ()
    {
        std::int64_t now = 0;
        auto domain = create (now);
        require_rejected (domain, 1, 0, "stale-sequence");
        require_rejected (domain, 1, Limits::max_safe_integer + 1, "integer-out-of-range");
        require (Limits::max_safe_integer == 9'007'199'254'740'991,
                 "The contract integer limit changed");
        require (Limits::max_safe_integer <= std::numeric_limits<std::int64_t>::max (),
                 "The maximum safe integer does not fit int64_t");
        const auto gap = domain.move ("player", MoveRequest{1, 2});
        require (gap.accepted && gap.sequence == 2,
                 "A new sequence greater than the last processed sequence was rejected");

        ZonePositionDomain max_domain ("zone-2", [] { return 0; });
        max_domain.add_player (
          "last", EnterZone{Position{100}, Limits::max_safe_integer, 0});
        require_rejected (max_domain, 1, Limits::max_safe_integer, "stale-sequence", "last");
        require_rejected (max_domain, 1, std::numeric_limits<std::int64_t>::max (),
                          "integer-out-of-range", "last");

        ZonePositionDomain safe_arrival ("zone-2", [] { return 0; });
        require (!safe_arrival.queue_arrival (
                   "safe", EnterZone{Position{100}, Limits::max_safe_integer, 0}),
                 "The maximum safe sequence was rejected during Zone admission");
        ZonePositionDomain unsafe_arrival ("zone-2", [] { return 0; });
        const auto arrival_rejection = unsafe_arrival.queue_arrival (
          "unsafe", EnterZone{Position{100}, Limits::max_safe_integer + 1, 0});
        EnterZone arrival;
        require (arrival_rejection && arrival_rejection->reason == "integer-out-of-range"
                   && !unsafe_arrival.take_arrival ("unsafe", arrival),
                 "An out-of-range admission sequence was accepted or stored");
    }
    void resolves_initial_location ()
    {
        const auto valid = ZonePositionDomain::resolve_location (299);
        require (valid.zoneId == "zone-3" && !valid.rejection,
                 "An in-world initial position resolved to the wrong Zone");
        require (ZonePositionDomain::resolve_location (
                   std::numeric_limits<double>::quiet_NaN ()).rejection->reason == "non-finite",
                 "A non-finite initial position was not rejected");
        require (ZonePositionDomain::resolve_location (
                   std::numeric_limits<double>::infinity ()).rejection->reason == "non-finite",
                 "An infinite initial position was not rejected");
        require (ZonePositionDomain::resolve_location (Names::world_width).rejection->reason
                   == "outside-world",
                 "An initial position at the world limit was not rejected");
    }
    void restores_remaining_budget ()
    {
        std::int64_t source_now = 0;
        auto source = create (source_now);
        const auto accepted = source.move ("player", MoveRequest{ZonePositionDomain::MaxStep, 1});
        require (accepted.accepted, "Expected initial move to be accepted");
        source_now += 75;
        const auto snapshot = source.capture ();

        std::int64_t destination_now = 500;
        ZonePositionDomain destination ("zone-2", [&destination_now] { return destination_now; });
        destination.restore (snapshot);
        const auto rejected = destination.move ("player", MoveRequest{5, 2});
        require (rejected.rejection.has_value (), "Expected move-budget rejection after relocation");
        require (rejected.rejection->reason == "move-budget",
                 "Relocation lost the remaining movement budget");
        require (rejected.rejection->retryAfterMs == 25,
                 "Relocation did not preserve remaining milliseconds");
    }
    void tracks_presence_lifecycle ()
    {
        std::int64_t now = 0;
        auto domain = create (now);
        domain.add_player ("observer", EnterZone{Position{101}, 0, 0});
        require (std::ranges::distance (domain.connected_players ()) == 2,
                 "Connected players were not tracked by the Zone");
        const auto before = domain.state ("player");
        const auto disconnected = domain.set_connected ("player", false);
        require (disconnected && !disconnected->connected, "Disconnect did not end visibility");
        require (std::ranges::distance (domain.connected_players ()) == 1,
                 "Disconnected players remained visible");
        require (domain.state ("player").position == before.position,
                 "Disconnect erased the stored position");
        const auto reconnected = domain.set_connected ("player", true);
        require (reconnected && reconnected->connected, "Reconnect did not restore visibility");
        require (domain.state ("player").position == before.position,
                 "Reconnect changed the stored position");
        const auto left = domain.remove_player ("player");
        require (left && left->position == before.position, "Zone leave lost the last position");
        require (std::ranges::distance (domain.connected_players ()) == 1,
                 "Zone leave retained the player");
    }
    void preserves_json_contract ()
    {
        const auto left = nlohmann::json (PlayerLeft{"player", PlayerLeftReason::left_zone}).dump ();
        require (left.find ("\"reason\":\"left-zone\"") != std::string::npos,
                 "PlayerLeft reason was not serialized as its kebab-case name");
        const auto border = nlohmann::json (BorderPresence{
                               "zone-2", "player", Position{10}, 1,
                               BorderPresenceKind::moved, std::nullopt}).dump ();
        require (border.find ("\"kind\":\"moved\"") != std::string::npos,
                 "Border event kind was not serialized as its string name");
        require (border.find ("\"reason\":null") != std::string::npos,
                 "A nullable border reason was omitted instead of encoded as null");
        require (nlohmann::json::parse (border).get<BorderPresence> ().reason == std::nullopt,
                 "A nullable border reason did not round-trip");
        const auto rejected = nlohmann::json (ActionRejected{"outside-world", 0}).dump ();
        require (rejected.find ("\"reason\":\"outside-world\"") != std::string::npos,
                 "ActionRejected reason field name changed");
        require (rejected.find ("\"retryAfterMs\":0") != std::string::npos,
                 "ActionRejected retryAfterMs field name changed");
        require ((std::is_same_v<decltype (ActionRejected::retryAfterMs), std::int32_t>),
                 "ActionRejected RetryAfterMs must use a 32-bit integer");
    }

  public:
    void run ()
    {
        check ("non-finite movement", [&] { rejects_non_finite_movement (); });
        check ("step limit", [&] { rejects_oversized_step (); });
        check ("world boundary", [&] { rejects_outside_world (); });
        check ("movement budget", [&] { rejects_until_server_retry (); });
        check ("stale and unsafe sequence", [&] { rejects_invalid_sequence (); });
        check ("initial location resolution", [&] { resolves_initial_location (); });
        check ("remaining-time relocation snapshot", [&] { restores_remaining_budget (); });
        check ("presence lifecycle", [&] { tracks_presence_lifecycle (); });
        check ("enum and nullable JSON", [&] { preserves_json_contract (); });
        std::cout << "Domain checks: " << passed_ << "/9 passed; assertions: " << assertions_
                  << std::endl;
    }
};
inline void run_domain_checks () { DomainChecks{}.run (); }
}
