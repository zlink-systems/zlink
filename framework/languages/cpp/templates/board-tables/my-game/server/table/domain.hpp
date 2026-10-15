#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <numeric>
#include <optional>
#include <random>

namespace MyGame
{
struct Seat
{
    std::string playerId;
    int chips = Rules::initial_chips;
    std::vector<int> cards;
    bool acted = false;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (Seat, playerId, chips, cards, acted)
};
class TableState
{
  public:
    std::vector<Seat> seats;
    std::vector<int> deck;
    int handNumber = 0, pot = 0, turnSeat = Rules::no_seat;
    bool inHand = false;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (TableState, seats, deck, handNumber, pot, turnSeat, inHand)
    int seat_of (std::string_view id) const
    {
        for (std::size_t i = 0; i < seats.size (); ++i)
            if (seats[i].playerId == id)
                return static_cast<int> (i);
        return Rules::no_seat;
    }
    // Seats the player at admission; a seated player is admitted again without a new seat.
    bool admit (std::string_view id)
    {
        if (seat_of (id) != Rules::no_seat)
            return true;
        if (seats.size () == Rules::seats)
            return false;
        seats.push_back (Seat{std::string (id), Rules::initial_chips, {}, false});
        return true;
    }
    std::vector<SeatView> views () const
    {
        std::vector<SeatView> result;
        result.reserve (seats.size ());
        for (std::size_t i = 0; i < seats.size (); ++i) {
            const auto &s = seats[i];
            result.push_back ({static_cast<int> (i), s.playerId, s.chips, s.acted,
                               static_cast<int> (s.cards.size ())});
        }
        return result;
    }
    TableView
    view (std::string_view player, const std::string &table, const std::string &node) const
    {
        return {table, node, seat_of (player), handNumber, inHand, turnSeat, pot, views ()};
    }
    std::string start (std::string_view id)
    {
        if (seat_of (id) == Rules::no_seat)
            return Rules::not_seated;
        if (inHand)
            return Rules::hand_in_progress;
        if (seats.size () < Rules::minimum_players)
            return Rules::not_enough_players;
        deck.resize (Rules::deck_size);
        std::iota (deck.begin (), deck.end (), 0);
        std::mt19937 random (std::random_device{}());
        std::shuffle (deck.begin (), deck.end (), random);
        for (auto &seat : seats) {
            seat.cards.clear ();
            seat.acted = false;
            for (int i = 0; i < Rules::cards_per_hand; ++i) {
                seat.cards.push_back (deck.back ());
                deck.pop_back ();
            }
        }
        ++handNumber;
        pot = 0;
        turnSeat = 0;
        inHand = true;
        return {};
    }
    struct BetOutcome
    {
        std::string rejection;
        std::optional<HandResult> result;
    };
    BetOutcome bet (std::string_view id, const BetRequest &request)
    {
        const auto index = seat_of (id);
        if (index == Rules::no_seat)
            return {Rules::not_seated, {}};
        if (!inHand || request.handNumber != handNumber)
            return {Rules::no_hand, {}};
        if (index != turnSeat)
            return {Rules::not_your_turn, {}};
        auto &seat = seats[index];
        if (request.amount < 0 || request.amount > seat.chips)
            return {Rules::invalid_amount, {}};
        seat.chips -= request.amount;
        pot += request.amount;
        seat.acted = true;
        for (std::size_t i = index + 1; i < seats.size (); ++i)
            if (!seats[i].cards.empty () && !seats[i].acted) {
                turnSeat = static_cast<int> (i);
                return {};
            }
        HandResult result;
        result.handNumber = handNumber;
        result.pot = pot;
        int best = -1;
        for (std::size_t i = 0; i < seats.size (); ++i) {
            const auto &s = seats[i];
            if (s.cards.empty ())
                continue;
            result.hands.push_back ({static_cast<int> (i), s.playerId, s.cards});
            int score = 0;
            for (int card : s.cards)
                score += card % Rules::ranks + Rules::lowest_rank;
            if (score > best) {
                best = score;
                result.winners.clear ();
            }
            if (score == best)
                result.winners.push_back (static_cast<int> (i));
        }
        const auto count = static_cast<int> (result.winners.size ());
        for (int winner : result.winners)
            seats[winner].chips += pot / count;
        seats[result.winners.front ()].chips += pot % count;
        inHand = false;
        turnSeat = Rules::no_seat;
        pot = 0;
        result.seats = views ();
        return {{}, std::move (result)};
    }
};
}
