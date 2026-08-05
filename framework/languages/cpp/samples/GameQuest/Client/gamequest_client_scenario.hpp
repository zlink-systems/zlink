/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

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
#include <thread>

namespace zlink::samples::gamequest
{

class gamequest_client_scenario_t
{
  public:
    bool run (const std::string &api_a_stream_endpoint,
              const std::string &api_b_stream_endpoint,
              const std::string &api_a_http_url,
              const std::string &api_b_http_url)
    {
        try {
            auto api_a_core = connect (api_a_stream_endpoint);
            auto api_b_core = connect (api_b_stream_endpoint);
            auto api_a = zlink::stream_e2e_client::use (api_a_core);
            auto api_b = zlink::stream_e2e_client::use (api_b_core);

            auto joined = api_a.request (join_session_req_t{"player-alice"})
                            .packet_name (join_session_req_t::packet_name)
                            .async<join_session_res_t> ()
                            .result ();
            dump_initial_join_quests ("player-alice", joined ? joined.value ().active_quests
                                                             : std::vector<quest_progress_t>{});
            ensure (joined && joined.value ().active_quests.empty (),
                    "player-alice initial join should have no active quests");

            auto first_progress = api_a.wait_for<quest_progress_notify_t> ()
                                    .where ([] (const quest_progress_notify_t &notify) {
                                        return notify.player_id == "player-alice"
                                               && notify.progress.quest_id
                                                    == quest_ids_t::first_hunt
                                               && notify.progress.current_count == 1;
                                    })
                                    .timeout (std::chrono::seconds (12))
                                    .to_future ("first hunt progress wait failed");
            auto first_kill = api_a.request (
                                  kill_monster_req_t{"player-alice", "wolf", "forest", "kill-1"})
                                .packet_name (kill_monster_req_t::packet_name)
                                .async<kill_monster_res_t> ()
                                .result ();
            dump_event_id_if_mismatch ("first kill", "player-alice-kill-1", first_kill);
            ensure (first_kill && first_kill.value ().event_id == "player-alice-kill-1",
                    "first kill event id mismatch");
            ensure (first_progress.get ().progress.current_count == 1,
                    "first hunt progress push mismatch");

            auto first_hunt_completed = api_a.wait_for<quest_completed_notify_t> ()
                                          .where ([] (const quest_completed_notify_t &notify) {
                                              return notify.player_id == "player-alice"
                                                     && notify.progress.quest_id
                                                          == quest_ids_t::first_hunt
                                                     && notify.reward_granted;
                                          })
                                          .timeout (std::chrono::seconds (12))
                                          .to_future ("first hunt completion wait failed");
            (void) api_a.request (
                       kill_monster_req_t{"player-alice", "wolf", "forest", "kill-2"})
              .packet_name (kill_monster_req_t::packet_name)
              .async<kill_monster_res_t> ()
              .result ();
            auto third_kill = api_a.request (
                                  kill_monster_req_t{"player-alice", "wolf", "forest", "kill-3"})
                                .packet_name (kill_monster_req_t::packet_name)
                                .async<kill_monster_res_t> ()
                                .result ();
            ensure (third_kill && third_kill.value ().event_id == "player-alice-kill-3",
                    "third kill event id mismatch");
            ensure (first_hunt_completed.get ().progress.status == quest_status_t::reward_granted,
                    "first hunt completion push mismatch");

            auto duplicate = api_a.request (
                                  kill_monster_req_t{"player-alice", "wolf", "forest", "kill-3"})
                               .packet_name (kill_monster_req_t::packet_name)
                               .async<kill_monster_res_t> ()
                               .result ();
            ensure (duplicate && duplicate.value ().event_id == third_kill.value ().event_id,
                    "duplicate kill idempotency mismatch");

            api_a.send (collect_item_req_t{"player-bob", "healing-herb", 1, "herb-1"}).submit ();

            auto bob_joined = api_b.request (join_session_req_t{"player-bob"})
                                .packet_name (join_session_req_t::packet_name)
                                .async<join_session_res_t> ()
                                .result ();
            ensure (static_cast<bool> (bob_joined), "player-bob join failed");
            /* gameplay event는 one-way라 offline 동안 쌓인 진행이 join 시점에 이미 반영돼 있다는
             * 보장은 없다. 조회로 반영을 기다린다. */
            ensure (wait_for_progress (api_b, "player-bob", quest_ids_t::herb_gathering, 1),
                    "player-bob did not see the offline herb progress");

            auto herb_completed = api_b.wait_for<quest_completed_notify_t> ()
                                    .where ([] (const quest_completed_notify_t &notify) {
                                        return notify.player_id == "player-bob"
                                               && notify.progress.quest_id
                                                    == quest_ids_t::herb_gathering
                                               && notify.reward_granted;
                                    })
                                    .timeout (std::chrono::seconds (12))
                                    .to_future ("herb completion wait failed");
            api_b.send (collect_item_req_t{"player-bob", "healing-herb", 4, "herb-2"}).submit ();
            ensure (herb_completed.get ().progress.status == quest_status_t::reward_granted,
                    "herb completion push mismatch");

            /* projection 재생성(§14): 표시용 projection을 지운 뒤 event stream만으로 같은
             * RewardGranted 상태를 복원한다. */
            auto deleted = api_b.request (
                                  projection_admin_req_t{"player-bob",
                                                         quest_ids_t::herb_gathering,
                                                         "delete"})
                             .packet_name (projection_admin_req_t::packet_name)
                             .async<projection_admin_res_t> ()
                             .result ();
            ensure (deleted && deleted.value ().ok,
                    "player-bob projection delete failed");
            auto missing_projection = api_b.request (get_quest_progress_req_t{"player-bob"})
                                        .packet_name (get_quest_progress_req_t::packet_name)
                                        .async<get_quest_progress_res_t> ()
                                        .result ();
            ensure (missing_projection
                      && std::none_of (missing_projection.value ().active_quests.begin (),
                                       missing_projection.value ().active_quests.end (),
                                       [] (const quest_progress_t &progress) {
                                           return progress.quest_id
                                                  == quest_ids_t::herb_gathering;
                                       }),
                    "deleted player-bob projection is still visible");
            auto rebuilt = api_b.request (
                                  projection_admin_req_t{"player-bob",
                                                         quest_ids_t::herb_gathering,
                                                         "rebuild"})
                             .packet_name (projection_admin_req_t::packet_name)
                             .async<projection_admin_res_t> ()
                             .result ();
            ensure (rebuilt && rebuilt.value ().ok
                      && std::any_of (rebuilt.value ().projection.begin (),
                                      rebuilt.value ().projection.end (),
                                      [] (const quest_progress_t &progress) {
                                          return progress.quest_id
                                                   == quest_ids_t::herb_gathering
                                                 && progress.status
                                                      == quest_status_t::reward_granted;
                                      }),
                    "player-bob projection rebuild did not replay the event stream");

            /* reward 멱등(§14): 완료된 quest에 같은 gameplay event를 다시 적용해도 진행이 더
             * 오르지 않고 상태도 그대로다. */
            auto replayed_kill =
              api_a.request (kill_monster_req_t{"player-alice", "wolf", "forest", "kill-3"})
                .packet_name (kill_monster_req_t::packet_name)
                .async<kill_monster_res_t> ()
                .result ();
            ensure (replayed_kill && replayed_kill.value ().event_id == "player-alice-kill-3",
                    "replayed kill event id mismatch");
            auto after_replay = api_a.request (get_quest_progress_req_t{"player-alice"})
                                  .packet_name (get_quest_progress_req_t::packet_name)
                                  .async<get_quest_progress_res_t> ()
                                  .result ();
            ensure (after_replay
                      && has_progress (after_replay.value ().active_quests,
                                       quest_ids_t::first_hunt, 3),
                    "replayed gameplay event changed the quest progress");

            /* reset/reconcile(§14): owner에게 publish되지 않은 authoritative gameplay fact를
             * GameApi에만 기록한 뒤 Sync로 QuestReconciled event를 만든다. */
            auto unpublished = api_a.request (unpublished_kill_req_t{"player-alice", 1})
                                 .packet_name (unpublished_kill_req_t::packet_name)
                                 .async<unpublished_kill_res_t> ()
                                 .result ();
            ensure (unpublished && unpublished.value ().ok,
                    "unpublished gameplay fact setup failed");
            auto reconciled = api_a.request (sync_quest_progress_req_t{"player-alice"})
                                .packet_name (sync_quest_progress_req_t::packet_name)
                                .async<sync_quest_progress_res_t> ()
                                .result ();
            ensure (reconciled
                      && has_progress (reconciled.value ().updated_quests,
                                       quest_ids_t::first_hunt, 4),
                    "gameplay snapshot did not reconcile the first-hunt progress");

            /* reconnect(§14): 다른 노드로 다시 접속하면 notify가 그 노드로 따라온다. session
             * binding이 owner spot의 notify 경로를 정하기 때문이다. */
            auto alice_b_core = connect (api_b_stream_endpoint);
            auto alice_b = zlink::stream_e2e_client::use (alice_b_core);
            auto alice_rejoined = alice_b.request (join_session_req_t{"player-alice"})
                                    .packet_name (join_session_req_t::packet_name)
                                    .async<join_session_res_t> ()
                                    .result ();
            ensure (static_cast<bool> (alice_rejoined),
                    "player-alice rejoin on the second node failed");
            auto ruins_completed = alice_b.wait_for<quest_completed_notify_t> ()
                                     .where ([] (const quest_completed_notify_t &notify) {
                                         return notify.player_id == "player-alice"
                                                && notify.progress.quest_id
                                                     == quest_ids_t::visit_ruins;
                                     })
                                     .timeout (std::chrono::seconds (12))
                                     .to_future ("ruins completion wait after reconnect failed");
            alice_b.send (enter_area_req_t{"player-alice", "ruins", "enter-ruins"}).submit ();
            ensure (ruins_completed.get ().progress.status == quest_status_t::reward_granted,
                    "reconnected player did not receive the notify on the new node");

            /* owner deactivate→reactivate(§14): Spot을 닫고 다시 생성했을 때 event stream
             * replay로 앞선 진행이 복원된다. */
            auto deactivated = alice_b.request (
                                     projection_admin_req_t{"player-alice", "", "deactivate"})
                                .packet_name (projection_admin_req_t::packet_name)
                                .async<projection_admin_res_t> ()
                                .result ();
            ensure (deactivated && deactivated.value ().ok,
                    "player-alice owner deactivation failed");
            std::this_thread::sleep_for (std::chrono::milliseconds (500));
            auto rehydrated = alice_b.request (get_quest_progress_req_t{"player-alice"})
                                .packet_name (get_quest_progress_req_t::packet_name)
                                .async<get_quest_progress_res_t> ()
                                .result ();
            ensure (rehydrated
                      && has_progress (rehydrated.value ().active_quests,
                                       quest_ids_t::first_hunt, 4),
                    "owner reactivation did not rehydrate the event stream");

            /* scale-out (§18): two independent player owners exercise both
             * API streams and let the RouteMesh placement select the two
             * QuestMission owners. The client observes only typed progress;
             * owner placement remains a server-side concern. */
            auto scale_a_core = connect (api_a_stream_endpoint);
            auto scale_b_core = connect (api_b_stream_endpoint);
            auto scale_a = zlink::stream_e2e_client::use (scale_a_core);
            auto scale_b = zlink::stream_e2e_client::use (scale_b_core);
            auto scale_a_joined = scale_a.request (join_session_req_t{"player-scale-a"})
                                    .packet_name (join_session_req_t::packet_name)
                                    .async<join_session_res_t> ()
                                    .result ();
            auto scale_b_joined = scale_b.request (join_session_req_t{"player-scale-b"})
                                    .packet_name (join_session_req_t::packet_name)
                                    .async<join_session_res_t> ()
                                    .result ();
            ensure (scale_a_joined && scale_b_joined,
                    "GameQuest scale-out player join failed");
            auto scale_a_progress = scale_a.wait_for<quest_progress_notify_t> ()
                                      .where ([] (const quest_progress_notify_t &notify) {
                                          return notify.player_id == "player-scale-a"
                                                 && notify.progress.current_count == 1;
                                      })
                                      .timeout (std::chrono::seconds (12))
                                      .to_future ("scale-a progress wait failed");
            auto scale_b_progress = scale_b.wait_for<quest_progress_notify_t> ()
                                      .where ([] (const quest_progress_notify_t &notify) {
                                          return notify.player_id == "player-scale-b"
                                                 && notify.progress.current_count == 1;
                                      })
                                      .timeout (std::chrono::seconds (12))
                                      .to_future ("scale-b progress wait failed");
            auto scale_a_event = scale_a.request (
                                      kill_monster_req_t{"player-scale-a", "wolf", "forest",
                                                         "scale-kill-1"})
                                   .packet_name (kill_monster_req_t::packet_name)
                                   .async<kill_monster_res_t> ()
                                   .result ();
            scale_b.send (collect_item_req_t{"player-scale-b", "healing-herb", 1, "scale-herb-1"})
              .submit ();
            ensure (scale_a_event && scale_a_event.value ().event_id == "player-scale-a-scale-kill-1",
                    "GameQuest scale-out event id mismatch");
            ensure (scale_a_progress.get ().progress.current_count == 1
                      && scale_b_progress.get ().progress.current_count == 1,
                    "GameQuest scale-out progress push mismatch");

            assert_server (api_a_http_url);
            assert_server (api_b_http_url);
            std::cout << "gamequest-server-evidence=completed\n";
            std::cout << "gamequest=completed\n";
            return true;
        }
        catch (const std::exception &error) {
            std::cerr << "gamequest scenario failed: " << error.what () << "\n";
            return false;
        }
    }

  private:
    using connector_t = zlink::stream_e2e_client::coroutine_connector_t;

    static zlink::stream_connector::connector_t connect (const std::string &endpoint)
    {
        zlink::stream_connector::connector_options_t options;
        options.endpoint = endpoint;
        options.connect_timeout = std::chrono::seconds (5);
        options.request_timeout = std::chrono::seconds (12);
        options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto core = zlink::stream_connector::connector_factory_t::create (options);
        auto connected = core.connect ();
        ensure (static_cast<bool> (connected), "stream connect failed");
        return core;
    }

    static bool has_progress (const std::vector<quest_progress_t> &projection,
                              const std::string &quest_id,
                              int current_count)
    {
        return std::any_of (projection.begin (), projection.end (),
                            [&] (const quest_progress_t &progress) {
                                return progress.quest_id == quest_id
                                       && progress.current_count == current_count;
                            });
    }

    static void dump_initial_join_quests (const std::string &player_id,
                                          const std::vector<quest_progress_t> &quests)
    {
        std::cerr << "gamequest initial join dump player=" << player_id
                  << " activeQuestCount=" << quests.size () << "\n";
        for (const auto &quest : quests) {
            std::cerr << "gamequest initial join quest"
                      << " questId=" << quest.quest_id
                      << " creatorPlayer=" << quest.player_id
                      << " lastEventId=" << quest.last_source_event_id.value_or ("<none>")
                      << " updatedAtUnixMs=" << quest.updated_at_unix_ms
                      << " status=" << quest.status
                      << " count=" << quest.current_count << "/"
                      << quest.required_count << "\n";
        }
    }

    template <typename TResult>
    static void dump_event_id_if_mismatch (const char *label,
                                           const std::string &expected,
                                           const TResult &result)
    {
        if (result && result.value ().event_id == expected) {
            return;
        }

        std::cerr << "gamequest event id dump label=" << label << " expected=" << expected;
        if (result) {
            std::cerr << " actual=" << result.value ().event_id;
        }
        else {
            std::cerr << " actual=<no-response>";
            if (result.error ()) {
                std::cerr << " error=" << result.error ()->message;
            }
        }
        std::cerr << "\n";
    }

    static void assert_server (const std::string &api_http_url)
    {
        auto assertion = zlink::http_client::client_t::create (api_http_url)
                           .timeout (std::chrono::seconds (12))
                           .build ()
                           .post ("/self-check/assert")
                           .body (server_assertion_req_t{})
                           .fetch<server_assertion_res_t> ();
        ensure (assertion.passed, "server assertion failed");
    }

    template <typename TClient>
    static bool wait_for_progress (TClient &client,
                                   const std::string &player_id,
                                   const std::string &quest_id,
                                   int expected_count)
    {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (12);
        while (std::chrono::steady_clock::now () < deadline) {
            auto current = client.request (get_quest_progress_req_t{player_id})
                             .packet_name (get_quest_progress_req_t::packet_name)
                             .template async<get_quest_progress_res_t> ()
                             .result ();
            if (current && has_progress (current.value ().active_quests, quest_id, expected_count)) {
                return true;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
        return false;
    }

    static void ensure (bool condition, const char *message)
    {
        if (!condition) {
            throw std::runtime_error (message);
        }
    }
};

} // namespace zlink::samples::gamequest
