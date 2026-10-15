#pragma once
#include "client.hpp"
#include <numeric>

namespace MyGame::E2E
{
struct Bets
{
    static constexpr int table_first = 100, table_second = 50, continuity_first = 10,
                         continuity_second = 20;
};
inline void conservation (const std::vector<SeatView> &seats)
{
    const int total = std::accumulate (seats.begin (), seats.end (), 0,
                                       [] (int n, const auto &s) { return n + s.chips; });
    require (total == Rules::initial_chips * Rules::minimum_players, "Chips were not conserved");
}
inline std::pair<HandDealt, HandDealt> start (Client &a, Client &b)
{
    a.clear ();
    b.clear ();
    a.send (StartHandRequest{});
    const auto av = a.next<TableView> ([] (const auto &v) { return v.inHand; });
    const auto bv = b.next<TableView> ([] (const auto &v) { return v.inHand; });
    require (av.turnSeat == 0 && bv.turnSeat == 0 && av.handNumber == bv.handNumber,
             "Invalid initial hand turn");
    auto ah = a.next<HandDealt> (), bh = b.next<HandDealt> ();
    require (ah.handNumber == av.handNumber && bh.handNumber == av.handNumber,
             "Hand number mismatch");
    require (ah.cards.size () == Rules::cards_per_hand && bh.cards.size () == Rules::cards_per_hand,
             "Expected two private cards");
    std::set<int> cards;
    for (const auto &hand : {ah, bh})
        for (int card : hand.cards) {
            require (card >= 0 && card < Rules::deck_size, "Invalid card");
            require (cards.insert (card).second, "Duplicate card");
        }
    return {ah, bh};
}
inline void
settle (Client &a, Client &b, const HandDealt &ah, const HandDealt &bh, int first, int second)
{
    a.privacy (ah);
    b.privacy (bh);
    b.send (BetRequest{bh.handNumber, second});
    const auto ar = a.next<HandResult> (), br = b.next<HandResult> ();
    require (ar == br && ar.handNumber == ah.handNumber && ar.pot == first + second,
             "Result differs between clients");
    require (ar.hands.size () == Rules::minimum_players, "Missing revealed hands");
    require (ar.hands[0].playerId == a.player_id && ar.hands[0].seat == 0
               && ar.hands[0].cards == ah.cards && ar.hands[1].playerId == b.player_id
               && ar.hands[1].seat == 1 && ar.hands[1].cards == bh.cards,
             "Revealed cards differ from deal");
    const auto score = [] (const auto &hand) {
        int sum = 0;
        for (int card : hand.cards)
            sum += card % Rules::ranks + Rules::lowest_rank;
        return sum;
    };
    std::vector<int> winners;
    if (score (ah) >= score (bh))
        winners.push_back (0);
    if (score (bh) >= score (ah))
        winners.push_back (1);
    require (ar.winners == winners, "Incorrect winners");
    std::vector<int> chips{Rules::initial_chips - first, Rules::initial_chips - second};
    for (int winner : winners)
        chips[winner] += ar.pot / static_cast<int> (winners.size ());
    chips[winners.front ()] += ar.pot % static_cast<int> (winners.size ());
    require (ar.seats.size () == chips.size (), "Seat count changed");
    for (std::size_t i = 0; i < chips.size (); ++i)
        require (ar.seats[i].chips == chips[i], "Incorrect settlement");
    conservation (ar.seats);
    a.healthy ();
    b.healthy ();
}
inline void initial (Client &a, Client &b)
{
    const auto av = a.join (), bv = b.join ();
    require (av.yourSeat == 0 && bv.yourSeat == 1, "Lowest empty seat was not allocated");
    require (bv.seats.size () == Rules::minimum_players, "Missing seated players");
    for (const auto &s : bv.seats)
        require (s.chips == Rules::initial_chips, "Invalid initial chips");
}
inline void table (const std::string &endpoint)
{
    const auto id = unique_id ();
    Client a (endpoint, "p1-" + id, "table-" + id), b (endpoint, "p2-" + id, "table-" + id);
    initial (a, b);
    const auto [ah, bh] = start (a, b);
    b.send (BetRequest{bh.handNumber, Bets::table_second});
    require (b.next<ActionRejected> ().reason == Rules::not_your_turn,
             "Out-of-turn bet not rejected");
    a.send (BetRequest{ah.handNumber, Bets::table_first});
    const auto state =
      b.next<TableView> ([] (const auto &v) { return v.pot == Bets::table_first; });
    require (state.turnSeat == 1
               && state.seats[0].chips == Rules::initial_chips - Bets::table_first,
             "First bet state incorrect");
    settle (a, b, ah, bh, Bets::table_first, Bets::table_second);
    a.close ();
    b.close ();
}
struct Continuity
{
    HandDealt a, b;
    TableView before;
};
inline Continuity in_progress (Client &a, Client &b)
{
    initial (a, b);
    const auto [ah, bh] = start (a, b);
    a.send (BetRequest{ah.handNumber, Bets::continuity_first});
    auto before =
      b.next<TableView> ([] (const auto &v) { return v.pot == Bets::continuity_first; });
    require (before.turnSeat == 1, "Second player must own turn");
    return {ah, bh, before};
}
inline void same_state (const TableView &before, const TableView &after)
{
    require (before.tableId == after.tableId && before.yourSeat == after.yourSeat
               && before.handNumber == after.handNumber && before.inHand == after.inHand
               && before.turnSeat == after.turnSeat && before.pot == after.pot
               && before.seats == after.seats,
             "Game state changed during continuity");
}
inline void reconnect (const std::string &endpoint)
{
    const auto id = unique_id (), table_id = "table-" + id, player = "p2-" + id;
    Client a (endpoint, "p1-" + id, table_id), b (endpoint, player, table_id);
    const auto state = in_progress (a, b);
    const auto before = b.info (), a_info = a.info ();
    b.close ();
    Client replacement (endpoint, player, table_id);
    const auto view = replacement.join ();
    const auto after = replacement.info ();
    require (after.sessionId != before.sessionId && after.actorId == before.actorId
               && after.objectGeneration == before.objectGeneration,
             "Reconnect did not preserve Actor identity");
    same_state (state.before, view);
    require (replacement.next<HandDealt> () == state.b, "Reconnect changed private cards");
    require (a.info () == a_info, "Other client identity changed");
    settle (a, replacement, state.a, state.b, Bets::continuity_first, Bets::continuity_second);
    a.close ();
    replacement.close ();
}
inline void update (const std::string &endpoint, World &world)
{
    const auto id = unique_id (), table_id = "table-" + id;
    Client a (endpoint, "p1-" + id, table_id), b (endpoint, "p2-" + id, table_id);
    const auto state = in_progress (a, b);
    const auto ai = a.info (), bi = b.info ();
    auto &source = world.table_node (state.before.nodeId);
    source.command (Control::drain);
    source.wait_line (Control::requested, Limits::relocation);
    source.wait_line (Control::relocated, Limits::relocation);
    source.wait_line (Control::safe, Limits::relocation);
    require (a.info () == ai && b.info () == bi, "Drain changed client identity");
    a.clear ();
    b.clear ();
    b.send (TableViewRequest{});
    const auto after = b.next<TableView> ();
    require (after.nodeId != state.before.nodeId, "Table did not relocate");
    same_state (state.before, after);
    require (b.next<HandDealt> () == state.b, "Drain changed private cards");
    a.send (TableViewRequest{});
    a.next<TableView> ();
    require (a.next<HandDealt> () == state.a, "Drain changed first player's cards");
    settle (a, b, state.a, state.b, Bets::continuity_first, Bets::continuity_second);
    world.replace_table (state.before.nodeId);
    a.close ();
    b.close ();
    table (endpoint);
}
}
