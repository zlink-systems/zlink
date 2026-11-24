#pragma once
#include "client.hpp"
#include "server/table/domain.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <set>

#include "test_json.hpp"

namespace MyGame::E2E
{
struct Bets
{
    static constexpr std::int64_t table_first = 100, table_second = 50, continuity_first = 10,
                                  continuity_second = 20;
};
inline constexpr int domain_check_count = 7;
inline int domain_checks ()
{
    int passed = 0;
    constexpr auto maximum = Rules::max_contract_integer;
    {
        TableState state;
        require (state.admit ("p1") && state.admit ("p2"),
                 "Could not prepare shuffled deck uniqueness check");
        require (state.start ("p1").empty (), "Deck shuffle failed");
        std::set<int> cards (state.deck.begin (), state.deck.end ());
        std::size_t card_count = state.deck.size ();
        for (const auto &seat : state.seats) {
            card_count += seat.cards.size ();
            cards.insert (seat.cards.begin (), seat.cards.end ());
        }
        int expected_card = 0;
        const bool is_complete_deck =
          cards.size () == Rules::deck_size
          && std::all_of (cards.begin (), cards.end (),
                          [&expected_card] (int card) { return card == expected_card++; });
        require (card_count == Rules::deck_size && is_complete_deck,
                 "Shuffled deck did not contain every card exactly once");
    }
    ++passed;

    {
        TableState state ([] { return std::numeric_limits<std::uint32_t>::max (); });
        require (state.admit ("p1") && state.admit ("p2"),
                 "Could not prepare deterministic shuffle");
        require (state.start ("p1").empty (), "Injected random source failed");
        const std::vector<int> expected_order{2,  1,  4,  9,  14, 40, 12, 7,  34, 18, 22, 13, 8,
                                              33, 16, 19, 50, 24, 37, 23, 25, 27, 49, 28, 20, 26,
                                              48, 44, 29, 39, 32, 31, 35, 17, 10, 46, 6,  5,  21,
                                              42, 36, 43, 51, 3,  30, 11, 41, 15, 38, 45, 0,  47};
        constexpr auto dealt_count = Rules::minimum_players * Rules::cards_per_hand;
        const std::vector<int> expected_remaining (expected_order.begin (),
                                                   expected_order.end () - dealt_count);
        const std::vector<int> expected_first_hand{expected_order[51], expected_order[50]};
        const std::vector<int> expected_second_hand{expected_order[49], expected_order[48]};
        require (state.deck == expected_remaining && state.seats[0].cards == expected_first_hand
                   && state.seats[1].cards == expected_second_hand,
                 "Injected random source produced a different deterministic order");
    }
    ++passed;

    {
        TableState state;
        for (int i = 0; i < Rules::seats; ++i)
            require (state.admit ("p" + std::to_string (i)), "Could not prepare settlement cards");
        state.handNumber = 1;
        state.inHand = true;
        state.turnSeat = 0;
        for (std::size_t i = 0; i < state.seats.size (); ++i)
            state.seats[i].cards = {static_cast<int> (i), static_cast<int> (i + Rules::seats)};
        std::optional<HandResult> result;
        for (int i = 0; i < Rules::seats; ++i) {
            const auto outcome = state.bet ("p" + std::to_string (i), BetRequest{1, 0});
            require (outcome.rejection.empty (), "Valid settlement bet was rejected");
            if (i + 1 == Rules::seats)
                result = outcome.result;
            else
                require (!outcome.result, "Settlement ended before every player acted");
        }
        require (result.has_value () && result->seats.size () == Rules::seats
                   && std::all_of (result->seats.begin (), result->seats.end (),
                                   [] (const auto &seat) { return seat.cardCount == 0; })
                   && std::all_of (state.seats.begin (), state.seats.end (),
                                   [] (const auto &seat) { return seat.cards.empty (); }),
                 "Settlement retained private cards");
    }
    ++passed;

    {
        TableState state;
        require (state.admit ("p1") && state.admit ("p2"), "Could not prepare bet amount boundary");
        state.seats[0].chips = maximum;
        state.seats[0].cards = {11, 24};
        state.seats[1].cards = {0, 13};
        state.handNumber = 1;
        state.inHand = true;
        state.turnSeat = 0;
        const auto accepted = state.bet ("p1", BetRequest{1, maximum});
        require (accepted.rejection.empty () && !accepted.result && state.pot == maximum
                   && state.seats[0].chips == 0,
                 "Maximum in-range bet was not accepted");
        const nlohmann::json before = test_json_board_scenarios::object (state);
        const auto outcome = state.bet ("p2", BetRequest{1, maximum + 1});
        require (outcome.rejection == Rules::integer_out_of_range,
                 "Over-limit bet amount did not return integer-out-of-range");
        require (test_json_board_scenarios::object (state) == before,
                 "Over-limit bet rejection changed state");
    }
    ++passed;

    {
        TableState state;
        require (state.admit ("p1") && state.admit ("p2"), "Could not prepare pot boundary");
        state.seats[0].chips = maximum;
        state.seats[0].cards = {11, 24};
        state.seats[1].cards = {0, 13};
        state.handNumber = 1;
        state.inHand = true;
        state.turnSeat = 0;
        state.pot = maximum;
        const nlohmann::json before = test_json_board_scenarios::object (state);
        const auto outcome = state.bet ("p1", BetRequest{1, 1});
        require (outcome.rejection == Rules::integer_out_of_range,
                 "Pot overflow did not return integer-out-of-range");
        require (test_json_board_scenarios::object (state) == before,
                 "Pot overflow rejection changed state");
    }
    ++passed;

    {
        TableState state;
        require (state.admit ("p1") && state.admit ("p2"), "Could not prepare payout boundary");
        state.seats[0].chips = maximum;
        state.seats[0].cards = {11, 24};
        state.seats[1].cards = {0, 13};
        state.handNumber = 1;
        state.inHand = true;
        state.turnSeat = 0;
        const auto first = state.bet ("p1", BetRequest{1, 0});
        require (first.rejection.empty () && !first.result,
                 "First payout-boundary bet ended the hand");
        const nlohmann::json before = test_json_board_scenarios::object (state);
        const auto outcome = state.bet ("p2", BetRequest{1, 1});
        require (outcome.rejection == Rules::integer_out_of_range,
                 "Payout overflow did not return integer-out-of-range");
        require (test_json_board_scenarios::object (state) == before,
                 "Payout overflow rejection changed state");
    }
    ++passed;

    {
        TableState state;
        require (state.admit ("p1") && state.admit ("p2"),
                 "Could not prepare hand-number boundary");
        state.handNumber = maximum;
        const nlohmann::json before = test_json_board_scenarios::object (state);
        require (state.start ("p1") == Rules::integer_out_of_range,
                 "Maximum hand number did not reject the next hand");
        require (test_json_board_scenarios::object (state) == before,
                 "Hand number overflow rejection changed state");
    }
    ++passed;
    return passed;
}
inline void conservation (const std::vector<SeatView> &seats)
{
    const auto total = std::accumulate (seats.begin (), seats.end (), std::int64_t{0},
                                        [] (std::int64_t n, const auto &s) { return n + s.chips; });
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
inline TableView
first_bet (Client &first, Client &second, const HandDealt &hand, std::int64_t amount)
{
    first.send (BetRequest{hand.handNumber, amount});
    const auto view = second.next<TableView> ([amount] (const auto &v) { return v.pot == amount; });
    require (view.turnSeat == 1 && view.seats[0].chips == Rules::initial_chips - amount,
             "First bet state incorrect");
    return view;
}
inline void settle (Client &a,
                    Client &b,
                    const HandDealt &ah,
                    const HandDealt &bh,
                    std::int64_t first,
                    std::int64_t second)
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
    std::vector<std::int64_t> chips{Rules::initial_chips - first, Rules::initial_chips - second};
    for (int winner : winners)
        chips[winner] += ar.pot / static_cast<int> (winners.size ());
    chips[winners.front ()] += ar.pot % static_cast<int> (winners.size ());
    require (ar.seats.size () == chips.size (), "Seat count changed");
    for (std::size_t i = 0; i < chips.size (); ++i)
        require (ar.seats[i].chips == chips[i], "Incorrect settlement");
    require (std::all_of (ar.seats.begin (), ar.seats.end (),
                          [] (const auto &seat) { return seat.cardCount == 0; }),
             "Settlement retained a private card count");
    conservation (ar.seats);
    a.clear ();
    b.clear ();
    a.send (TableViewRequest{});
    const auto settled_a = a.next<TableView> ();
    const auto empty_a = a.next<HandDealt> ();
    b.send (TableViewRequest{});
    const auto settled_b = b.next<TableView> ();
    const auto empty_b = b.next<HandDealt> ();
    for (const auto &view : {settled_a, settled_b})
        require (!view.inHand && view.handNumber == ah.handNumber
                   && std::all_of (view.seats.begin (), view.seats.end (),
                                   [] (const auto &seat) { return seat.cardCount == 0; }),
                 "Settled table view retained private cards");
    require (empty_a.handNumber == ah.handNumber && empty_a.cards.empty ()
               && empty_b.handNumber == ah.handNumber && empty_b.cards.empty (),
             "Private-hand query returned cards after settlement");
    a.healthy ();
    b.healthy ();
}
inline void initial (Client &a, Client &b, const std::string &expected_node = {})
{
    const auto av = a.join (), bv = b.join ();
    if (!expected_node.empty ())
        require (av.nodeId == expected_node && bv.nodeId == expected_node,
                 "Replacement node did not host new work");
    require (av.yourSeat == 0 && bv.yourSeat == 1, "Lowest empty seat was not allocated");
    require (bv.seats.size () == Rules::minimum_players, "Missing seated players");
    for (const auto &s : bv.seats)
        require (s.chips == Rules::initial_chips, "Invalid initial chips");
}
inline void same_state (const TableView &before, const TableView &after)
{
    require (before.tableId == after.tableId && before.yourSeat == after.yourSeat
               && before.handNumber == after.handNumber && before.inHand == after.inHand
               && before.turnSeat == after.turnSeat && before.pot == after.pot
               && before.seats == after.seats,
             "Game state changed during continuity");
}
inline void table (const std::string &endpoint, const std::string &expected_node = {})
{
    const auto id = unique_id ();
    Client a (endpoint, "p1-" + id, "table-" + id), b (endpoint, "p2-" + id, "table-" + id);
    initial (a, b, expected_node);

    b.send (TableViewRequest{});
    const auto before_packet = b.next<TableView> ();
    require (b.next<HandDealt> ().cards.empty (), "Idle table returned private cards");
    b.send (HandResult{});
    const auto packet_rejection = b.next<ActionRejected> ();
    require (packet_rejection.reason == Rules::packet_not_allowed
               && packet_rejection.retryAfterMs == 0,
             "Internal packet was not rejected");
    b.send (TableViewRequest{});
    const auto after_packet = b.next<TableView> ();
    require (b.next<HandDealt> ().cards.empty (), "Packet rejection changed private cards");
    same_state (before_packet, after_packet);

    const auto [ah, bh] = start (a, b);
    b.send (TableViewRequest{});
    const auto before_out_of_turn = b.next<TableView> ();
    require (b.next<HandDealt> () == bh, "Table view changed private cards");
    b.send (BetRequest{bh.handNumber, Bets::table_second});
    const auto out_of_turn = b.next<ActionRejected> ();
    require (out_of_turn.reason == Rules::not_your_turn && out_of_turn.retryAfterMs == 0,
             "Out-of-turn bet rejection differs");
    b.send (TableViewRequest{});
    const auto after_out_of_turn = b.next<TableView> ();
    require (b.next<HandDealt> () == bh, "Rejected bet changed private cards");
    same_state (before_out_of_turn, after_out_of_turn);

    a.send (TableViewRequest{});
    const auto before_limit = a.next<TableView> ();
    require (a.next<HandDealt> () == ah, "Table view changed private cards");
    a.send (BetRequest{ah.handNumber, Rules::max_contract_integer + 1});
    const auto over_limit = a.next<ActionRejected> ();
    require (over_limit.reason == Rules::integer_out_of_range && over_limit.retryAfterMs == 0,
             "Over-limit bet rejection differs");
    a.send (TableViewRequest{});
    const auto after_limit = a.next<TableView> ();
    require (a.next<HandDealt> () == ah, "Rejected bet changed private cards");
    same_state (before_limit, after_limit);

    a.send (BetRequest{ah.handNumber, Rules::initial_chips + 1});
    const auto invalid_amount = a.next<ActionRejected> ();
    require (invalid_amount.reason == Rules::invalid_amount && invalid_amount.retryAfterMs == 0,
             "Invalid amount rejection differs");
    a.send (TableViewRequest{});
    const auto after_invalid_amount = a.next<TableView> ();
    require (a.next<HandDealt> () == ah, "Rejected bet changed private cards");
    same_state (before_limit, after_invalid_amount);

    first_bet (a, b, ah, Bets::table_first);
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
    const auto before = first_bet (a, b, ah, Bets::continuity_first);
    return {ah, bh, before};
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
template <typename Server> void update (const std::string &endpoint, Server &server)
{
    const auto id = unique_id (), table_id = "table-" + id;
    Client a (endpoint, "p1-" + id, table_id), b (endpoint, "p2-" + id, table_id);
    const auto state = in_progress (a, b);
    const auto ai = a.info (), bi = b.info ();
    server.drain_table (state.before.nodeId);
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
    server.replace_table (state.before.nodeId);
    a.close ();
    b.close ();
    table (endpoint, state.before.nodeId);
}
inline MatchQueued queue_match (Client &client)
{
    client.send (FindMatch{});
    const auto queued = client.next<MatchQueued> ();
    require (!queued.ticketId.empty (), "Missing match ticket");
    client.player_id = client.info ().actorId;
    return queued;
}

inline void play_match (const std::string &endpoint)
{
    Client a (endpoint, "", ""), b (endpoint, "", "");
    const auto qa = queue_match (a);
    const auto qb = queue_match (b);
    const auto a_match = a.next<MatchFound> ();
    const auto b_match = b.next<MatchFound> ();
    require (a_match.ticketId == qa.ticketId && b_match.ticketId == qb.ticketId
               && qa.ticketId != qb.ticketId,
             "MatchFound ticket must match that player's queue ticket");
    require (a_match.roomId.starts_with (MatchNames::room_prefix)
               && a_match.roomId == b_match.roomId && a_match.seat != b_match.seat,
             "The players must share one room and occupy different seats");
    for (auto *client : {&a, &b}) {
        const auto found = client == &a ? a_match : b_match;
        const auto view = client->next<TableView> ();
        require (view.tableId == found.roomId && view.yourSeat == found.seat
                   && view.seats.size () == Rules::minimum_players
                   && view.seats.at (static_cast<std::size_t> (found.seat)).playerId
                        == client->player_id,
                 "Committed table view must match the room and seats");
        client->require_match_sequence (found);
    }
    auto &first = a_match.seat == 0 ? a : b;
    auto &second = a_match.seat == 0 ? b : a;
    const auto [first_hand, second_hand] = start (first, second);
    first_bet (first, second, first_hand, Bets::table_first);
    settle (first, second, first_hand, second_hand, Bets::table_first, Bets::table_second);
    a.close ();
    b.close ();
}

inline void match_cancel (const std::string &endpoint)
{
    Client a (endpoint, "", "");
    const auto queued = queue_match (a);
    a.send (FindMatch{});
    require (a.next<MatchQueued> ().ticketId == queued.ticketId,
             "Duplicate FindMatch changed ticket");
    a.send (CancelMatch{queued.ticketId});
    const auto cancelled = a.next<MatchCancelled> ();
    require (cancelled.ticketId == queued.ticketId && cancelled.reason == MatchNames::cancelled,
             "Cancelled ticket and reason must match the request");
    play_match (endpoint);
    a.close ();
}
}
