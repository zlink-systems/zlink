/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "tictactoe_client_options.hpp"
#include "../Shared/Contracts/messages.hpp"

#include <zlink/http_client.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#define ensure(condition) require_condition ((condition), #condition)

namespace zlink::samples::tictactoe
{

class tictactoe_client_scenario_t
{
  public:
    bool run (stream_e2e_client::coroutine_connector_t &client1,
              stream_e2e_client::coroutine_connector_t &client2,
              const create_game_http_res_t &room,
              const tictactoe_client_options_t &options)
    {
        (void) client1;
        (void) client2;
        (void) room;
        (void) options;
        std::cerr << "tictactoe scenario requires a third observer client\n";
        return false;
    }

    bool run (const tictactoe_client_options_t &options)
    {
        try {
            const auto create_game_request = create_game_http_req_t{options.game_name};
            auto room = zlink::http_client::client_t::create (options.api_http_endpoint)
                          .post ("/games")
                          .timeout (std::chrono::seconds (45))
                          .body (create_game_request)
                          .fetch<create_game_http_res_t> ();
            if (room.play_endpoints.size () < 2) {
                throw std::runtime_error ("API must return at least two Play endpoints.");
            }
            ensure (room.play_nodes.size () == room.play_endpoints.size ());
            ensure (std::all_of (
              room.play_endpoints.begin (), room.play_endpoints.end (),
              [&room] (const std::string &endpoint) {
                  return std::count_if (
                           room.play_nodes.begin (), room.play_nodes.end (),
                           [&endpoint] (const play_node_info_t &node) {
                               return node.stream_endpoint == endpoint;
                           })
                         == 1;
              }));
            const auto owner_endpoint = room.play_endpoints.front ();
            std::string guest_endpoint;
            for (const auto &endpoint : room.play_endpoints) {
                if (endpoint != owner_endpoint) {
                    guest_endpoint = endpoint;
                    break;
                }
            }
            if (guest_endpoint.empty ()) {
                throw std::runtime_error ("API did not return a non-owner Play endpoint.");
            }

            zlink::stream_connector::connector_options_t connector_options;
            connector_options.endpoint = owner_endpoint;
            connector_options.connect_timeout = options.stream_timeout;
            connector_options.request_timeout = options.stream_timeout;
            connector_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
            auto guest_connector_options = connector_options;
            guest_connector_options.endpoint = guest_endpoint;

            auto core_client1 =
              zlink::stream_connector::connector_factory_t::create (connector_options);
            use_json_codec (core_client1);
            auto core_client2 =
              zlink::stream_connector::connector_factory_t::create (guest_connector_options);
            use_json_codec (core_client2);
            auto core_observer =
              zlink::stream_connector::connector_factory_t::create (guest_connector_options);
            use_json_codec (core_observer);
            [[maybe_unused]] auto inbound_log1 = core_client1.observe_inbound (
              [] (const zlink::stream_connector::inbound_observation_t &observation) {
                  std::cout << "stream-inbound sample=TicTacToe client=player kind="
                            << static_cast<int> (observation.kind) << " name=" << observation.name
                            << " seq="
                            << (observation.request_seq ? std::to_string (*observation.request_seq)
                                                        : std::string ("-"))
                            << " bytes=" << observation.payload_length << '\n';
              });
            [[maybe_unused]] auto inbound_log2 = core_client2.observe_inbound (
              [] (const zlink::stream_connector::inbound_observation_t &observation) {
                  std::cout << "stream-inbound sample=TicTacToe client=player kind="
                            << static_cast<int> (observation.kind) << " name=" << observation.name
                            << " seq="
                            << (observation.request_seq ? std::to_string (*observation.request_seq)
                                                        : std::string ("-"))
                            << " bytes=" << observation.payload_length << '\n';
              });

            [[maybe_unused]] auto inbound_log3 = core_observer.observe_inbound (
              [] (const zlink::stream_connector::inbound_observation_t &observation) {
                  std::cout << "stream-inbound sample=TicTacToe client=observer kind="
                            << static_cast<int> (observation.kind) << " name=" << observation.name
                            << " seq="
                            << (observation.request_seq ? std::to_string (*observation.request_seq)
                                                        : std::string ("-"))
                            << " bytes=" << observation.payload_length << '\n';
              });

            auto client1 = zlink::stream_e2e_client::use (core_client1);
            auto client2 = zlink::stream_e2e_client::use (core_client2);
            auto observer = zlink::stream_e2e_client::use (core_observer);
            if (!run_game (client1, client2, observer, room, options)) {
                return false;
            }

            auto recreate_client =
              zlink::stream_connector::connector_factory_t::create (connector_options);
            use_json_codec (recreate_client);
            [[maybe_unused]] auto recreate_inbound_log = recreate_client.observe_inbound (
              [] (const zlink::stream_connector::inbound_observation_t &observation) {
                  std::cout << "stream-inbound sample=TicTacToe client=player kind="
                            << static_cast<int> (observation.kind) << " name=" << observation.name
                            << " seq="
                            << (observation.request_seq ? std::to_string (*observation.request_seq)
                                                        : std::string ("-"))
                            << " bytes=" << observation.payload_length << '\n';
              });
            auto client3 = zlink::stream_e2e_client::use (recreate_client);
            return run_recreate_check (client3, options);
        }
        catch (const std::exception &ex) {
            std::cerr << "tictactoe scenario failed: " << ex.what () << '\n';
            return false;
        }
    }

  private:
    static bool run_game (stream_e2e_client::coroutine_connector_t &client1,
                          stream_e2e_client::coroutine_connector_t &client2,
                          stream_e2e_client::coroutine_connector_t &observer,
                          const create_game_http_res_t &room,
                          const tictactoe_client_options_t &options)
    {
        auto result = run_game_async (client1, client2, observer, room, options).result ();
        return result && result.value ();
    }

    static bool run_recreate_check (stream_e2e_client::coroutine_connector_t &client,
                                    const tictactoe_client_options_t &options)
    {
        auto result = run_recreate_check_async (client, options).result ();
        return result && result.value ();
    }

    static stream_e2e_client::task_t<bool>
    run_game_async (stream_e2e_client::coroutine_connector_t &client1,
                    stream_e2e_client::coroutine_connector_t &client2,
                    stream_e2e_client::coroutine_connector_t &observer,
                    const create_game_http_res_t &room,
                    const tictactoe_client_options_t &options)
    {
        try {
            ensure (!room.room_id.empty ());
            ensure (!room.room_id.empty ());
            ensure (room.play_endpoints.front ().rfind ("tcp://127.0.0.1:", 0) == 0);
            ensure (room.required_level == 3);
            ensure (room.game_name == options.game_name);

            trace ("connect client1");
            co_await client1.connect ().async ();
            trace ("connect client2");
            co_await client2.connect ().async ();
            trace ("connect observer");
            co_await observer.connect ().async ();
            std::cout << "observer-connected endpoint=" << non_owner_endpoint (room) << '\n';

            trace ("authenticate client1");
            const auto client1_auth_request = authenticate_req_t{options.x_actor_id};
            auto client1_auth =
              co_await client1.request (client1_auth_request)
                .async<authenticate_res_t> ();
            ensure (client1_auth.player.actor_id == options.x_actor_id);
            ensure (client1_auth.player.display_name == options.x_actor_id);
            ensure (client1_auth.player.level >= room.required_level);
            ensure (client1_auth.player.wins == 99);

            trace ("authenticate client2");
            const auto client2_auth_request = authenticate_req_t{options.o_actor_id};
            auto client2_auth =
              co_await client2.request (client2_auth_request)
                .async<authenticate_res_t> ();
            ensure (client2_auth.player.actor_id == options.o_actor_id);
            ensure (client2_auth.player.display_name == options.o_actor_id);
            ensure (client2_auth.player.level >= room.required_level);
            ensure (client2_auth.player.wins == 0);
            ensure (client2_auth.player.actor_id != client1_auth.player.actor_id);

            trace ("authenticate observer");
            const auto observer_auth_request = authenticate_req_t{options.observer_actor_id};
            auto observer_auth =
              co_await observer.request (observer_auth_request)
                .async<authenticate_res_t> ();
            ensure (observer_auth.player.actor_id == options.observer_actor_id);
            ensure (observer_auth.player.display_name == options.observer_actor_id);
            ensure (observer_auth.player.level > 0);
            ensure (observer_auth.player.wins == 0);
            const auto observe_request = observe_milestone_req_t{};
            auto observe = co_await observer.request (observe_request)
                             .async<observe_milestone_res_t> ();
            ensure (observe.subscribed);
            std::cout << "observer-subscription=verified subscribed=true\n";

            trace ("join client1");
            const auto client1_join_request = join_game_req_t{room.room_id};
            auto client1_join_completion =
              client1.wait_for<join_game_res_t> ().async ();
            client1.send (client1_join_request).submit ();
            auto client1_join = co_await client1_join_completion;
            ensure (client1_join.state.room_id == room.room_id);
            ensure (client1_join.state.x_actor_id == options.x_actor_id);
            ensure (!client1_join.state.o_actor_id);
            ensure (client1_join.state.status == tictactoe_status_t::waiting_for_players);
            ensure (client1_join.state.next_turn == tictactoe_marks_t::x);
            co_await client1.expect_none<player_joined_notify_t> ()
              .within (std::chrono::milliseconds (25))
              .async ();

            auto client1_wait_client2_join =
              client1.wait_for<player_joined_notify_t> ()
                .where (&player_joined_notify_t::actor_id, client2_auth.player.actor_id)
                .async ();
            auto client1_wait_game_start =
              client1.wait_for<game_state_notify_t> ()
                .where ([&options] (const game_state_notify_t &message) {
                    return message.state.status == tictactoe_status_t::in_progress
                           && message.state.o_actor_id == options.o_actor_id;
                })
                .async ();
            trace ("join client2");
            const auto client2_join_request = join_game_req_t{room.room_id};
            auto client2_join_completion =
              client2.wait_for<join_game_res_t> ().async ();
            client2.send (client2_join_request).submit ();
            auto client2_join = co_await client2_join_completion;
            ensure (client2_join.state.room_id == room.room_id);
            trace ((std::string ("client2_join state: x=")
                    + client2_join.state.x_actor_id.value_or ("<none>") + " o="
                    + client2_join.state.o_actor_id.value_or ("<none>") + " status="
                    + client2_join.state.status)
                     .c_str ());
            ensure (client2_join.state.o_actor_id == options.o_actor_id);
            ensure (client2_join.state.status == tictactoe_status_t::in_progress);
            co_await client2.expect_none<player_joined_notify_t> ()
              .within (std::chrono::milliseconds (25))
              .async ();
            trace ("wait client1 saw client2 join");
            auto client1_saw_client2_join = co_await client1_wait_client2_join;
            ensure (client1_saw_client2_join.actor_id == client2_auth.player.actor_id);
            ensure (client1_saw_client2_join.display_name == client2_auth.player.display_name);
            ensure (client1_saw_client2_join.level == client2_auth.player.level);
            ensure (client1_saw_client2_join.room_id == room.room_id);
            ensure (client1_saw_client2_join.mark == tictactoe_marks_t::o);
            ensure (client1_saw_client2_join.state.status
                    == tictactoe_status_t::in_progress);
            auto client1_saw_game_start = co_await client1_wait_game_start;
            ensure (client1_saw_game_start.room_id == room.room_id);
            ensure (client1_saw_game_start.state.status == tictactoe_status_t::in_progress);
            ensure (client1_saw_game_start.state.o_actor_id == options.o_actor_id);
            ensure (client1_saw_game_start.state.next_turn == tictactoe_marks_t::x);

            auto premature_terminal_state = client2.expect_none<game_state_notify_t> ()
                                              .within (std::chrono::milliseconds (250))
                                              .async ();
            client1.send (leave_game_msg_t{room.room_id}).submit ();
            co_await premature_terminal_state;

            auto client2_wait_first_move =
              client2.wait_for<game_state_notify_t> ()
                .where ([&options] (const game_state_notify_t &message) {
                    return message.state.last_move_actor_id == options.x_actor_id
                           && message.state.last_move_cell == 0;
                })
                .async ();
            trace ("client1 first move");
            const auto client1_first_move_request = place_mark_req_t{0};
            auto client1_first_move =
              co_await client1.request (client1_first_move_request)
                .async<place_mark_res_t> ();
            ensure (client1_first_move.state.room_id == room.room_id);
            ensure (client1_first_move.state.board == "X........");
            ensure (client1_first_move.state.next_turn == tictactoe_marks_t::o);
            ensure (client1_first_move.state.last_move_actor_id == options.x_actor_id);
            ensure (client1_first_move.state.last_move_cell == 0);
            trace ("client2 saw first move");
            auto client2_saw_first_move = co_await client2_wait_first_move;
            ensure (client2_saw_first_move.room_id == room.room_id);
            ensure (client2_saw_first_move.state.last_move_cell == 0);
            ensure (same_state (client2_saw_first_move.state, client1_first_move.state));

            auto client1_wait_first_o_move =
              client1.wait_for<game_state_notify_t> ()
                .where ([&options] (const game_state_notify_t &message) {
                    return message.state.last_move_actor_id == options.o_actor_id
                           && message.state.last_move_cell == 3;
                })
                .async ();
            trace ("client2 first move");
            const auto client2_first_move_request = place_mark_req_t{3};
            auto client2_first_move =
              co_await client2.request (client2_first_move_request)
                .async<place_mark_res_t> ();
            ensure (client2_first_move.state.room_id == room.room_id);
            ensure (client2_first_move.state.board == "X..O.....");
            ensure (client2_first_move.state.next_turn == tictactoe_marks_t::x);
            ensure (client2_first_move.state.last_move_actor_id == options.o_actor_id);
            ensure (client2_first_move.state.last_move_cell == 3);
            trace ("client1 saw first o move");
            auto client1_saw_first_o_move = co_await client1_wait_first_o_move;
            ensure (client1_saw_first_o_move.room_id == room.room_id);
            ensure (client1_saw_first_o_move.state.last_move_cell == 3);
            ensure (same_state (client1_saw_first_o_move.state, client2_first_move.state));

            auto client2_wait_second_x_move =
              client2.wait_for<game_state_notify_t> ()
                .where ([&options] (const game_state_notify_t &message) {
                    return message.state.last_move_actor_id == options.x_actor_id
                           && message.state.last_move_cell == 1;
                })
                .async ();
            trace ("client1 second move");
            const auto client1_second_move_request = place_mark_req_t{1};
            auto client1_second_move =
              co_await client1.request (client1_second_move_request)
                .async<place_mark_res_t> ();
            ensure (client1_second_move.state.room_id == room.room_id);
            ensure (client1_second_move.state.board == "XX.O.....");
            ensure (client1_second_move.state.next_turn == tictactoe_marks_t::o);
            ensure (client1_second_move.state.last_move_actor_id == options.x_actor_id);
            ensure (client1_second_move.state.last_move_cell == 1);
            trace ("client2 saw second x move");
            auto client2_saw_second_x_move = co_await client2_wait_second_x_move;
            ensure (client2_saw_second_x_move.room_id == room.room_id);
            ensure (client2_saw_second_x_move.state.last_move_cell == 1);
            ensure (same_state (client2_saw_second_x_move.state, client1_second_move.state));

            auto client1_wait_second_o_move =
              client1.wait_for<game_state_notify_t> ()
                .where ([&options] (const game_state_notify_t &message) {
                    return message.state.last_move_actor_id == options.o_actor_id
                           && message.state.last_move_cell == 4;
                })
                .async ();
            trace ("client2 second move");
            const auto client2_second_move_request = place_mark_req_t{4};
            auto client2_second_move =
              co_await client2.request (client2_second_move_request)
                .async<place_mark_res_t> ();
            ensure (client2_second_move.state.room_id == room.room_id);
            ensure (client2_second_move.state.board == "XX.OO....");
            ensure (client2_second_move.state.next_turn == tictactoe_marks_t::x);
            ensure (client2_second_move.state.last_move_actor_id == options.o_actor_id);
            ensure (client2_second_move.state.last_move_cell == 4);
            trace ("client1 saw second o move");
            auto client1_saw_second_o_move = co_await client1_wait_second_o_move;
            ensure (client1_saw_second_o_move.room_id == room.room_id);
            ensure (client1_saw_second_o_move.state.last_move_cell == 4);
            ensure (same_state (client1_saw_second_o_move.state, client2_second_move.state));

            auto client2_wait_winning_move =
              client2.wait_for<game_state_notify_t> ()
                .where ([&options] (const game_state_notify_t &message) {
                    return message.state.winner == options.x_actor_id;
                })
                .async ();
            auto observer_wait_milestone =
              observer.wait_for<win_milestone_notify_t> ()
                .where ([&options] (const win_milestone_notify_t &message) {
                    return message.actor_id == options.x_actor_id && message.wins == 100;
                })
                .async ();
            trace ("client1 winning move");
            const auto client1_winning_move_request = place_mark_req_t{2};
            auto client1_winning_move =
              co_await client1.request (client1_winning_move_request)
                .async<place_mark_res_t> ();
            ensure (client1_winning_move.state.room_id == room.room_id);
            ensure (client1_winning_move.state.last_move_actor_id == options.x_actor_id);
            ensure (client1_winning_move.state.last_move_cell == 2);
            ensure (client1_winning_move.state.board == "XXXOO....");
            ensure (client1_winning_move.state.status == tictactoe_status_t::won);
            ensure (client1_winning_move.state.winner == options.x_actor_id);
            trace ("client2 saw winning move");
            auto client2_saw_winning_move = co_await client2_wait_winning_move;
            ensure (client2_saw_winning_move.room_id == room.room_id);
            ensure (client2_saw_winning_move.state.board == "XXXOO....");
            ensure (client2_saw_winning_move.state.status == tictactoe_status_t::won);
            auto milestone = co_await observer_wait_milestone;
            ensure (milestone.room_id == room.room_id);
            ensure (milestone.actor_id == options.x_actor_id);
            ensure (milestone.display_name == client1_auth.player.display_name);
            ensure (milestone.wins == 100);
            std::cout << "observer-win-milestone=verified actor=" << milestone.actor_id
                      << " wins=" << milestone.wins << '\n';

            const auto client1_leave_request = leave_game_msg_t{room.room_id};
            client1.send (client1_leave_request).submit ();
            const auto client2_leave_request = leave_game_msg_t{room.room_id};
            client2.send (client2_leave_request).submit ();

            co_await client1.close ().async ();
            co_await client2.close ().async ();
            co_await observer.close ().async ();
            std::cout << "tictactoe completed\n";
            co_return true;
        }
        catch (const std::exception &ex) {
            std::cerr << "tictactoe game failed: " << ex.what () << '\n';
            (void) client1.close ();
            (void) client2.close ();
            (void) observer.close ();
            co_return false;
        }
    }

    static stream_e2e_client::task_t<bool>
    run_recreate_check_async (stream_e2e_client::coroutine_connector_t &client,
                              const tictactoe_client_options_t &options)
    {
        try {
            trace ("recreate connect");
            co_await client.connect ().async ();
            trace ("recreate authenticate");
            const auto recreate_auth_request = authenticate_req_t{options.x_actor_id};
            auto recreated =
              co_await client.request (recreate_auth_request)
                .async<authenticate_res_t> ();
            ensure (recreated.player.actor_id == options.x_actor_id);
            co_await client.close ().async ();
            co_return true;
        }
        catch (const std::exception &ex) {
            std::cerr << "tictactoe recreate failed: " << ex.what () << '\n';
            (void) client.close ();
            co_return false;
        }
    }

    static void require_condition (bool condition, const char *expression)
    {
        if (!condition) {
            throw std::runtime_error (std::string ("Ensure failed: ") + expression);
        }
    }

    static bool same_state (const tictactoe_state_t &left, const tictactoe_state_t &right)
    {
        return left.room_id == right.room_id && left.board == right.board
               && left.status == right.status && left.next_turn == right.next_turn
               && left.winner == right.winner
               && left.x_actor_id == right.x_actor_id && left.o_actor_id == right.o_actor_id
               && left.last_move_actor_id == right.last_move_actor_id
               && left.last_move_cell == right.last_move_cell;
    }

    static void trace (const char *step) { std::cerr << "tictactoe step: " << step << '\n'; }

    static void use_json_codec (zlink::stream_connector::connector_t &connector)
    {
        connector.codecs ()
          .enable_codec (zlink::stream_connector::codec_t::json)
          .use_default_codec (zlink::stream_connector::codec_t::json);
    }

    static std::string non_owner_endpoint (const create_game_http_res_t &room)
    {
        for (const auto &endpoint : room.play_endpoints) {
            if (endpoint != room.play_endpoints.front ()) {
                return endpoint;
            }
        }
        return {};
    }

};

#undef ensure

} // namespace zlink::samples::tictactoe
