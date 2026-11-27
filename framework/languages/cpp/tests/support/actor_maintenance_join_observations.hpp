/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include "runtime/diagnostics/dispatch_events.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/spots/spot.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
namespace fw = zlink::framework;
using namespace std::chrono_literals;

inline constexpr auto wait_budget = 5s;
inline constexpr auto default_follow_duration = 30s;
inline constexpr auto source_follow_duration = 3s;
inline constexpr int instance_probe = -1;
inline constexpr auto actor_type = "member";
inline constexpr auto actor_id = "member";
inline constexpr auto mesh_name = "maintenance-join";
inline constexpr auto room_type = "room";
inline constexpr auto first_rid = "join-first";
inline constexpr auto second_rid = "join-second";
inline constexpr auto third_rid = "join-third";
inline constexpr auto previous_spot = "previous";
inline constexpr auto maintenance_spot = "maintenance";
inline constexpr auto local_spot = "local";
inline constexpr auto current_spot = "current";
inline constexpr auto suffix_spot = "suffix-target";
inline constexpr auto failed_spot = "failed";
inline constexpr auto framework_failed_spot = "framework-failed";
inline constexpr auto rejected_spot = "rejected";
inline constexpr auto rejection_reply = "room is full";
enum class creation_case_t
{
    accepted,
    rejected,
    failed,
    publication_failed
};
inline constexpr auto entry_target = "entry";
// A second Actor type, so a node can register an Entry Spot that accepts only it.
inline constexpr auto other_actor_type = "other-member";

struct native_handoff_hold_t
{
    fw::task_completion_source_t<void> entered;
    fw::task_completion_source_t<void> preceding_h_finished;
    fw::task_completion_source_t<void> next_join_started;
    fw::task_completion_source_t<void> next_join_terminal;
    fw::task_completion_source_t<void> release;
    int entries = 0;
};
struct ordinary_spot_observation_t
{
    fw::task_completion_source_t<void> entered;
    fw::task_completion_source_t<void> release;
    std::vector<int> handled;
};
struct observations_t
{
    std::shared_ptr<ordinary_spot_observation_t> ordinary_spot;
    bool relay_completed = false;

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<fw::actor_join_completion_t> completions;
    std::vector<std::string> completion_owners;
    std::vector<std::optional<fw::spot_id_t>> completion_spots;
    bool leave_entered = false;
    bool materialization_entered = false;
    bool consecutive_join_probe = false;
    bool first_admission_accepted = false;
    bool first_capture_pending = false;
    bool second_admission_during_capture = false;
    int instances = 0;
    int room_factories = 0;
    creation_case_t creation_case = creation_case_t::accepted;
    int creation_callbacks = 0;
    int disconnect_callbacks = 0;
    std::shared_ptr<fw::task_completion_source_t<void>> disconnect_release;
    bool fail_remote_factory = false;
    int remote_factory_failure_count = 0;
    std::vector<int> destroyed;
    std::vector<std::pair<fw::spot_close_reason_t, std::size_t>> closings;
    std::function<fw::task_t<void> ()> closing_hook;
    int destroy_callbacks = 0;
    int entry_joined = 0;
    int entry_left = 0;
    std::atomic<fw::actor_client_t *> joined_sender{nullptr};
    std::function<fw::task_t<void> (fw::actor_id_t)> joined_view;
    std::function<fw::task_t<void> ()> join_completion_view;
    std::function<fw::task_t<void> ()> before_room_probe_reply;
    bool release_replay_post = false;
    bool next_join_attempted = false;
    bool send_after_close = false;
    std::string next_join_error;
    int session_pushes = 0;
    std::vector<int> joined_payloads;
    std::vector<int> handled_payloads;
    std::vector<int> intake_probe_values;
    std::vector<fw::message_flow_event_t> replay_failures;
    int backlog_appends = 0;
    int ordinary_posts = 0;
    bool fail_completion = false;
    bool transfer_suffix = false;
    bool suffix_request = false;
    std::function<fw::result_t<int> ()> suffix_reply;
    std::vector<std::string> handled_spots;
    std::vector<std::string> handled_nodes;
    std::vector<std::size_t> handled_completion_counts;
    bool hold_replay_prefix = false;
    bool replay_prefix_entered = false;
    bool release_replay_prefix = false;
    std::shared_ptr<native_handoff_hold_t> native_handoff_hold;
    fw::task_completion_source_t<void> joined_send_captured;
    struct joined_admission_batch_t
    {
        std::size_t remaining;
        std::function<void ()> complete;
    };
    std::optional<joined_admission_batch_t> joined_admission_completion;
    fw::task_completion_source_t<void> release_leave;
    bool hold_join_admission = false;
    std::function<void ()> after_move_deferred;
    std::optional<std::chrono::milliseconds> join_timeout;
    fw::task_completion_source_t<void> join_admission_entered;
    fw::task_completion_source_t<void> release_join_admission;
    fw::task_completion_source_t<void> join_completed;
    bool probe_admission_observed = false;
    std::optional<std::size_t> completion_to_hold;
    fw::task_completion_source_t<void> release_completion;
    std::vector<std::string> factory_nodes;
    std::vector<int> recorded;
};
} // namespace zlink::framework::test::actor_maintenance_join
