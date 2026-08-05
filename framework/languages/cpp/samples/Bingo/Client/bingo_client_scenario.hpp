/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "bingo_client_options.hpp"
#include "../Shared/Contracts/messages.hpp"
#include "../Shared/Contracts/protobuf_stream_codec.hpp"

#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define ensure(condition) ensure ((condition), #condition)

namespace zlink::samples::bingo
{

class bingo_client_scenario_t
{
  public:
    bool run (stream_e2e_client::coroutine_connector_t &client1,
              stream_e2e_client::coroutine_connector_t &client2,
              stream_e2e_client::coroutine_connector_t &observer)
    {
        auto result = run_async (client1, client2, observer).result ();
        return result && result.value ();
    }

#undef ensure
  private:
    static stream_e2e_client::task_t<bool>
    run_async (stream_e2e_client::coroutine_connector_t &client1,
               stream_e2e_client::coroutine_connector_t &client2,
               stream_e2e_client::coroutine_connector_t &observer)
    {
        try {
            const std::vector<int> client1_card_numbers{1, 2, 3, 4, 0, 6, 7, 8, 9};
            const std::vector<int> client2_card_numbers{10, 11, 12, 13, 0, 14, 4, 5, 6};

            trace ("connect client1");
            co_await client1.connect ().async ();
            trace ("connect client2");
            co_await client2.connect ().async ();
            trace ("connect observer");
            co_await observer.connect ().async ();

            /* 세 client를 인증한 뒤 global ActorId로 matching을 진행한다. Object owner는
             * Location Store가 결정하므로 application scenario가 NodeRid를 비교하지 않는다. */
            trace ("authenticate client1");
            const auto client1_auth_request = authenticate_req_t{bingo_sample_players_t::player1};
            auto client1_auth = co_await authenticate (client1, client1_auth_request);
            ensure (client1_auth.actor_id == bingo_sample_players_t::player1);

            trace ("authenticate client2");
            const auto client2_auth_request = authenticate_req_t{bingo_sample_players_t::player2};
            auto client2_auth = co_await authenticate (client2, client2_auth_request);
            ensure (client2_auth.actor_id == bingo_sample_players_t::player2);
            ensure (client2_auth.actor_id != client1_auth.actor_id);

            trace ("authenticate observer");
            const auto observer_auth_request = authenticate_req_t{bingo_sample_players_t::observer};
            auto observer_auth = co_await authenticate (observer, observer_auth_request);
            ensure (observer_auth.actor_id == bingo_sample_players_t::observer);

            trace ("match client1");
            const auto client1_match_request = match_bingo_req_t{bingo_sample_modes_t::two_player};
            auto client1_match =
              co_await client1.request (client1_match_request)
                .async<match_bingo_res_t> ();
            ensure (client1_match.state.status == bingo_room_status_t::waiting);
            ensure (client1_match.state.host_actor_id == client1_auth.actor_id);
            co_await client1.expect_none<player_joined_notify_t> ()
              .within (std::chrono::milliseconds (25))
              .async ();


            trace ("observe reward events");
            const auto observe_request = observe_bingo_events_req_t{client1_match.room_id};
            auto observed = co_await observer.request (observe_request)
                              .async<observe_bingo_events_res_t> ();
            ensure (observed.subscribed);

            trace ("match client2");
            auto client1_joined_task =
              client1.wait_for<player_joined_notify_t> ()
                .where (&player_joined_notify_t::actor_id, client2_auth.actor_id)
                .async ();
            auto client1_started_task = client1.wait_for<game_started_notify_t> ().async ();
            auto client2_started_task = client2.wait_for<game_started_notify_t> ().async ();
            const auto match_request = match_bingo_req_t{bingo_sample_modes_t::two_player};
            auto client2_match =
              co_await client2.request (match_request).async<match_bingo_res_t> ();
            ensure (client2_match.room_id == client1_match.room_id);
            // The join is deferred until the MatchBingo handler completes. The
            // GameStarted notification below is the public completion signal.
            ensure (client2_match.state.status == bingo_room_status_t::waiting);
            auto client1_joined = co_await client1_joined_task;
            auto client1_started = co_await client1_started_task;
            auto client2_started = co_await client2_started_task;
            co_await client2.expect_none<player_joined_notify_t> ()
              .within (std::chrono::milliseconds (25))
              .async ();
            const auto room_id = client1_match.room_id;
            trace ("client1 wait joined");
            ensure (client1_joined.actor_id == client2_auth.actor_id);
            ensure (std::all_of (
              client1_joined.state.players.begin (), client1_joined.state.players.end (),
              [] (const bingo_player_state_t &player) {
                  return player.wins == 0 && player.losses == 0;
              }));

            trace ("wait game started");
            ensure (client1_started.state.room_id == room_id);
            ensure (client1_started.state.status == bingo_room_status_t::running);
            ensure (client2_started.state.room_id == room_id);
            ensure (client2_started.state.status == bingo_room_status_t::running);
            ensure (client2_match.state.room_id == room_id);

            bool player_stop_observing_rejected = false;
            try {
                (void) co_await client1.request (stop_observing_bingo_events_req_t{room_id})
                  .async<stop_observing_bingo_events_res_t> ();
            }
            catch (const std::exception &) {
                player_stop_observing_rejected = true;
            }
            ensure (player_stop_observing_rejected,
                    "a game-room player must not stop an observer subscription");

            trace ("client2 submit card");
            const auto client2_card_request =
              submit_bingo_card_req_t{room_id, client2_card_numbers};
            auto client2_card =
              co_await client2.request (client2_card_request)
                .async<submit_bingo_card_res_t> ();
            ensure (client2_card.state.status == bingo_room_status_t::running);
            ensure (std::any_of (
              client2_card.state.players.begin (), client2_card.state.players.end (),
              [&client2_auth] (const bingo_player_state_t &player) {
                  return player.actor_id == client2_auth.actor_id && player.card.size () == 9;
              }));

            bool duplicate_card_rejected = false;
            try {
                (void) co_await client2.request (client2_card_request)
                  .async<submit_bingo_card_res_t> ();
            }
            catch (const std::exception &) {
                duplicate_card_rejected = true;
            }
            ensure (duplicate_card_rejected, "a player must not replace a submitted card");

            trace ("client1 submit card");
            constexpr int expected_draw_count = 3;
            auto reward_task = observer.wait_for<bingo_reward_announced_notify_t> ()
                                 .where (&bingo_reward_announced_notify_t::room_id, room_id)
                                 .async ();
            std::vector<zlink::stream_e2e_client::task_t<number_drawn_notify_t>>
              client1_draw_tasks;
            std::vector<zlink::stream_e2e_client::task_t<number_drawn_notify_t>>
              client2_draw_tasks;
            client1_draw_tasks.reserve (expected_draw_count);
            client2_draw_tasks.reserve (expected_draw_count);
            for (int draw_seq = 1; draw_seq <= expected_draw_count; ++draw_seq) {
                client1_draw_tasks.push_back (
                  client1.wait_for<number_drawn_notify_t> ()
                    .where (&number_drawn_notify_t::draw_seq, draw_seq)
                    .async ());
                client2_draw_tasks.push_back (
                  client2.wait_for<number_drawn_notify_t> ()
                    .where (&number_drawn_notify_t::draw_seq, draw_seq)
                    .async ());
            }
            auto client1_ended_task =
              client1.wait_for<game_ended_notify_t> ()
                .where ([] (const game_ended_notify_t &message) {
                    return message.state.status == bingo_room_status_t::finished;
                })
                .async ();
            auto client2_ended_task =
              client2.wait_for<game_ended_notify_t> ()
                .where ([] (const game_ended_notify_t &message) {
                    return message.state.status == bingo_room_status_t::finished;
                })
                .async ();
            const auto client1_card_request =
              submit_bingo_card_req_t{room_id, client1_card_numbers};
            auto client1_card =
              co_await client1.request (client1_card_request)
                .async<submit_bingo_card_res_t> ();
            // Drawing is server-driven after both cards arrive; the submit reply
            // still reflects the running game (same as the .NET scenario).
            ensure (client1_card.state.status == bingo_room_status_t::running);
            ensure (client1_card.state.players.size () == 2);
            ensure (std::all_of (
              client1_card.state.players.begin (), client1_card.state.players.end (),
              [] (const bingo_player_state_t &player) { return player.card.size () == 9; }));
            std::vector<number_drawn_notify_t> drawn_numbers;
            for (int draw_seq = 1; draw_seq <= expected_draw_count; ++draw_seq) {
                auto client1_drawn =
                  co_await client1_draw_tasks[static_cast<std::size_t> (draw_seq - 1)];
                auto client2_drawn =
                  co_await client2_draw_tasks[static_cast<std::size_t> (draw_seq - 1)];
                drawn_numbers.push_back (client1_drawn);
                ensure (client1_drawn.draw_seq == draw_seq);
                ensure (client2_drawn.draw_seq == draw_seq);
                ensure (client2_drawn.number == client1_drawn.number);
                ensure (same_bingo_room_state (client1_drawn.state, client2_drawn.state));
            }
            ensure (drawn_numbers.size () == expected_draw_count);
            ensure (drawn_numbers.back ().state.status == bingo_room_status_t::finished);
            auto client1_ended = co_await client1_ended_task;
            auto client2_ended = co_await client2_ended_task;
            ensure (client1_ended.state.status == bingo_room_status_t::finished);
            ensure (client2_ended.state.status == bingo_room_status_t::finished);
            ensure (client2_ended.state.drawn_numbers == client1_ended.state.drawn_numbers);
            ensure (client2_ended.state.winners == client1_ended.state.winners);
            ensure (same_bingo_player_list (client1_ended.state.players,
                                            client2_ended.state.players));
            ensure (client1_ended.state.drawn_numbers.size () == drawn_numbers.size ());
            for (std::size_t index = 0; index < drawn_numbers.size (); ++index) {
                ensure (client1_ended.state.drawn_numbers[index] == drawn_numbers[index].number);
            }
            // Final results are validated on the pushed game-ended state, matching
            // the .NET scenario: winners, full cards, and the marked free cell.
            ensure (!client1_ended.state.drawn_numbers.empty ());
            ensure (client1_ended.state.winners
                    == std::vector<std::string>{client1_auth.actor_id});
            ensure (std::all_of (
              client1_ended.state.players.begin (), client1_ended.state.players.end (),
              [] (const bingo_player_state_t &player) {
                  return player.card.size () == 9 && player.marks.size () == 9 && player.marks[4];
              }));

            trace ("wait reward announcement");
            auto reward = co_await reward_task;
            ensure (reward.actor_id == client1_auth.actor_id);
            ensure (reward.draw_seq == client1_ended.state.draw_seq);
            ensure (reward.item_id == bingo_reward_items_t::golden_dauber_id);
            ensure (reward.item_name == bingo_reward_items_t::golden_dauber_name);
            ensure (reward.rarity == bingo_reward_items_t::legendary_rarity);

            trace ("stop observing");
            const auto stop_observing_request = stop_observing_bingo_events_req_t{room_id};
            auto stopped =
              co_await observer.request (stop_observing_request)
                .async<stop_observing_bingo_events_res_t> ();
            trace ("stop observing completed");
            ensure (stopped.stopped);

            co_await client1.close ().async ();
            co_await client2.close ().async ();
            co_await observer.close ().async ();
            co_return true;
        }
        catch (const std::exception &ex) {
            std::cerr << "bingo game failed: " << ex.what () << '\n';
            (void) client1.close ();
            (void) client2.close ();
            (void) observer.close ();
            co_return false;
        }
    }

    static void ensure (bool condition, const char *expression)
    {
        if (!condition) {
            throw std::runtime_error (std::string ("Ensure failed: ") + expression);
        }
    }

    static bool same_bingo_player_list (const std::vector<bingo_player_state_t> &left,
                                        const std::vector<bingo_player_state_t> &right)
    {
        return left.size () == right.size ()
               && std::equal (
                 left.begin (), left.end (), right.begin (), [] (const auto &a, const auto &b) {
                     return a.actor_id == b.actor_id && a.display_name == b.display_name
                            && a.seat == b.seat && a.is_host == b.is_host && a.card == b.card
                            && a.marks == b.marks && a.completed_lines == b.completed_lines
                            && a.wins == b.wins && a.losses == b.losses;
                 });
    }

    static bool same_bingo_room_state (const bingo_room_state_t &left,
                                       const bingo_room_state_t &right)
    {
        return left.room_id == right.room_id && left.status == right.status
               && left.host_actor_id == right.host_actor_id && left.can_start == right.can_start
               && left.draw_seq == right.draw_seq
               && left.last_drawn_number == right.last_drawn_number
               && left.drawn_numbers == right.drawn_numbers
               && same_bingo_player_list (left.players, right.players)
               && left.winners == right.winners;
    }

    static stream_e2e_client::task_t<authenticate_res_t>
    authenticate (stream_e2e_client::coroutine_connector_t &client,
                  const authenticate_req_t &request)
    {
        co_return co_await client.request (request).async<authenticate_res_t> ();
    }

    static void ensure (bool condition,
                        std::source_location location = std::source_location::current ())
    {
        ensure (condition, ("condition at " + std::string (location.file_name ()) + ":"
                            + std::to_string (location.line ()))
                             .c_str ());
    }

    static void trace (const char *step) { std::cerr << "bingo step: " << step << '\n'; }
};

} // namespace zlink::samples::bingo
