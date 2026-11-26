#pragma once
#include "scenarios.hpp"

namespace MyGame::E2E
{
inline void persist (const std::string &endpoint, World &fresh_world)
{
    const auto id = unique_id (), table_id = "table-" + id;
    Client a (endpoint, "p1-" + id, table_id), b (endpoint, "p2-" + id, table_id);
    const auto playing = in_progress (a, b);
    const auto result =
      settle (a, b, playing.a, playing.b, Bets::continuity_first, Bets::continuity_second);
    const auto &balance = result.seats.front ();
    require (balance.chips != Rules::initial_chips, "The settled hand did not change the balance");
    a.close ();
    b.close ();

    const auto fresh_endpoint = fresh_world.start ();
    Client stale (fresh_endpoint, balance.playerId, table_id);
    stale.send (JoinTableRequest{balance.playerId, table_id, true});
    const auto rejected = stale.next<ActionRejected> ();
    require (rejected.reason == Rules::not_seated && rejected.retryAfterMs == 0,
             "The old seat survived the location state reset");
    stale.close ();

    Client fresh (fresh_endpoint, balance.playerId, "table-" + unique_id ());
    const auto view = fresh.join ();
    require (view.seats.size () == 1 && view.yourSeat == 0
               && view.seats.front ().playerId == balance.playerId
               && fresh.info ().actorId == balance.playerId,
             "The recreated Actor or seat has the wrong player ID");
    require (view.seats.front ().chips == balance.chips,
             "The recreated Actor did not load the settled balance");
    fresh.close ();
}
}
