/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/manual_internal_scheduler.hpp"
#include <zlink/framework.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include <array>
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/stream_connector.hpp>
#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <functional>
#if defined(__linux__)
#include <filesystem>
#include <set>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <map>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace zlink::framework::detail
{
struct spot_node_runtime_test_access_t
{
    static task_t<void> replay (spot_node_runtime_t &runtime,
                                actor_ref_t actor,
                                std::vector<handoff_packet_t> packets,
                                service_provider_t services)
    {
        return runtime.replay_actor_handoff_batch (std::move (actor), std::move (packets),
                                                   std::move (services), "canonical-replay", false,
                                                   {});
    }
};
}

namespace zlink::framework::runtime::stateful
{
struct raw_stateful_dispatch_test_access_t
{
    static std::array<std::size_t, 3> spot_records (raw_stateful_dispatch_t &raw)
    {
        std::lock_guard lock (raw._mutex);
        std::array<std::size_t, 3> counts{};
        for (const auto &[key, pending] : raw._pending) {
            if (key.kind == object_kind_t::actor)
                continue;
            ++counts[0]; // A committed pending entry has an accepted frozen body.
            ++counts[1]; // The same entry stores its frozen summary.
        }
        for (const auto &[key, next] : raw._next_sequence)
            if (key.first != object_kind_t::actor)
                counts[2] += next - 1;
        return counts;
    }
};
}

namespace zlink::framework::runtime::host
{
// Reproduce a superseded assembly at the activation boundary without timing races.
struct public_host_runtime_test_access_t
{
    static std::array<std::size_t, 3> spot_records (public_host_runtime_t &host)
    {
        return stateful::raw_stateful_dispatch_test_access_t::spot_records (
          *host._stateful_dispatch);
    }
    static task_t<bool> restore_accepted_spot (public_host_runtime_t &host,
                                               std::shared_ptr<location_repository_t> store,
                                               const protocol::request_source_fence_t &source,
                                               int &materialized)
    {
        host._user_spot_store = std::move (store);
        host._objects.configure_relocation_state (
          [] (const stateful::object_ref_t &owner, const std::string &type, std::stop_token) {
              return stateful::frozen_object_state_t{owner, type};
          },
          [&materialized] (const auto &, const auto &, std::stop_token) {
              ++materialized;
              return true;
          });
        auto pending = std::make_shared<public_host_runtime_t::pending_relocation_assembly_t> (
          public_host_runtime_t::pending_relocation_assembly_t{
            .assembly = stateful::relocation_state_assembly_t{{}, 0, {}, {}, {}}});
        auto staging = std::make_shared<public_host_runtime_t::relocation_assembly_staging_t> ();
        stateful::object_ref_t owner{stateful::object_kind_t::user_spot,
                                     "accepted-spot",
                                     1,
                                     1,
                                     "source-boundary",
                                     "source-node"};
        auto target = owner;
        target.node_id = "target-node";
        target.authority_owner_generation = 2;
        protocol::frozen_record_t record;
        record.kind = protocol::frozen_record_kind_t::spot_send;
        record.source = source;
        record.operation = {1, 1};
        record.application =
          protocol::application_payload_t{"Probe", "application/json", {'{', '}'}};
        stateful::turn_record_t turn;
        turn.sequence = 1;
        turn.frozen_record = std::move (record);
        staging->frozen.push_back ({owner, "room", {}, {std::move (turn)}, {}});
        staging->sources.push_back (owner);
        staging->targets.push_back (target);
        staging->wire_objects.push_back ({});
        staging->restore_identity.reference = "accepted-source-boundary";
        return host.restore_relocation_assembly (pending, staging);
    }
    static task_t<bool> activate_stale_actor_attempt (public_host_runtime_t &host,
                                                      bool replaced,
                                                      bool activation_started,
                                                      const stateful::object_ref_t &target,
                                                      std::string stable_type)
    {
        const public_host_runtime_t::relocation_attempt_key_t key{1, 2, 3};
        auto pending = std::make_shared<public_host_runtime_t::pending_relocation_assembly_t> (
          public_host_runtime_t::pending_relocation_assembly_t{
            .assembly = stateful::relocation_state_assembly_t{{}, 0, {}, {}, {}}});
        pending->prepare.relocation = {1, 2};
        pending->prepare.target_attempt_generation = 3;
        auto staging = std::make_shared<public_host_runtime_t::relocation_assembly_staging_t> ();
        staging->targets.push_back (target);
        staging->wire_objects.emplace_back ();
        staging->wire_objects.back ().stable_type = std::move (stable_type);
        host._relocation_session_terminal_lane
          .run ([&] {
              auto &attempt = host._relocation_target_attempts[key];
              attempt.prepare = pending->prepare;
              attempt.pending = pending;
              attempt.terminal = std::make_shared<task_completion_source_t<void>> ();
          })
          .get ();
        std::promise<void> release_index;
        auto release_index_ready = release_index.get_future ();
        std::promise<void> index_entered;
        auto index = std::async (std::launch::async, [&] {
            host._spot_actor_index_lane
              .run ([&] {
                  index_entered.set_value ();
                  release_index_ready.wait ();
              })
              .get ();
        });
        struct release_and_join_t
        {
            std::promise<void> &release;
            std::future<void> &worker;
            ~release_and_join_t ()
            {
                if (worker.valid ()) {
                    release.set_value ();
                    worker.wait ();
                }
            }
        } release_and_join{release_index, index};
        index_entered.get_future ().get ();
        std::optional<task_t<bool>> activated;
        if (activation_started)
            activated.emplace (host.activate_relocation_assembly (key, pending, staging));
        auto discarded =
          host._relocation_session_terminal_lane
            .run ([&] {
                auto &attempt = host._relocation_target_attempts.at (key);
                auto cleanup = host.take_relocation_target_discard (attempt);
                if (!cleanup)
                    attempt.terminal->complete (result_t<void>::success ());
                if (replaced) {
                    attempt = public_host_runtime_t::relocation_target_attempt_t{};
                    attempt.pending =
                      std::make_shared<public_host_runtime_t::pending_relocation_assembly_t> (
                        *pending);
                } else
                    host._relocation_target_attempts.erase (key);
                return cleanup;
            })
            .get ();
        if (!activation_started)
            activated.emplace (host.activate_relocation_assembly (key, pending, staging));
        release_index.set_value ();
        index.get ();
        if (discarded) {
            std::vector<public_host_runtime_t::relocation_target_attempt_t> cleanup;
            cleanup.push_back (std::move (*discarded));
            host.discard_relocation_target_attempts (std::move (cleanup));
        }
        co_return co_await *activated;
    }
};
} // namespace zlink::framework::runtime::host

namespace
{
namespace fw = zlink::framework;
using namespace std::chrono_literals;
constexpr auto wait_budget = 5s;
constexpr auto default_follow_duration = 30s;
constexpr auto source_follow_duration = 3s;
constexpr std::size_t status_observation_capacity = 32;
constexpr int instance_probe = -1;
constexpr auto actor_type = "member";
constexpr auto actor_id = "member";
constexpr auto mesh_name = "maintenance-join";
constexpr auto room_type = "room";
constexpr auto first_rid = "join-first";
constexpr auto second_rid = "join-second";
constexpr auto third_rid = "join-third";
constexpr auto previous_spot = "previous";
constexpr auto maintenance_spot = "maintenance";
constexpr auto source_spot = "source";
constexpr auto local_spot = "local";
constexpr auto current_spot = "current";
constexpr auto suffix_spot = "suffix-target";
constexpr auto failed_spot = "failed";
constexpr auto factory_failed_spot = "factory-failed";
constexpr auto framework_failed_spot = "framework-failed";
constexpr auto rejected_spot = "rejected";
constexpr auto rejection_reply = "room is full";
enum class creation_case_t
{
    accepted,
    rejected,
    failed,
    publication_failed
};
constexpr auto entry_target = "entry";
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
    creation_case_t creation_case = creation_case_t::accepted;
    int creation_callbacks = 0;
    bool fail_remote_factory = false;
    int remote_factory_failure_count = 0;
    std::vector<int> destroyed;
    std::vector<std::pair<fw::spot_close_reason_t, std::size_t>> closings;
    int destroy_callbacks = 0;
    int entry_joined = 0;
    int entry_left = 0;
    std::atomic<fw::actor_client_t *> joined_sender{nullptr};
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
    fw::task_completion_source_t<void> join_admission_entered;
    fw::task_completion_source_t<void> release_join_admission;
    fw::task_completion_source_t<void> join_completed;
    bool probe_admission_observed = false;
    std::optional<std::size_t> completion_to_hold;
    fw::task_completion_source_t<void> release_completion;
    std::vector<std::string> factory_nodes;
    std::vector<int> recorded;
} *observations;
struct move_t
{
    static constexpr const char *packet_name = "maintenance-join-move";
    std::string target;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (move_t, target)
struct probe_t
{
    static constexpr const char *packet_name = "maintenance-join-probe";
    int value{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (probe_t, value)
struct ready_t
{
    static constexpr const char *packet_name = "maintenance-join-ready";
    int value{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (ready_t, value)
struct view_request_t
{
    static constexpr const char *packet_name = "maintenance-join-view-request";
    int value{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (view_request_t, value)
struct view_t
{
    static constexpr const char *packet_name = "maintenance-join-view";
    int instance{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (view_t, instance)
struct record_t
{
    static constexpr const char *packet_name = "maintenance-join-record";
    int value{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (record_t, value)
class actor_t final : public fw::actor_t
{
  public:
    explicit actor_t (fw::actor_context_t context) : _context (std::move (context))
    {
        std::lock_guard lock (observations->mutex);
        instance = ++observations->instances;
    }
    ~actor_t () override
    {
        {
            std::lock_guard lock (observations->mutex);
            observations->destroyed.push_back (instance);
        }
        observations->changed.notify_all ();
    }
    int instance;
    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }
    fw::task_t<void> on_join_completed (const fw::actor_join_completion_t &completion) override
    {
        bool hold = false;
        std::shared_ptr<native_handoff_hold_t> held_suffix;
        const auto current_spot = _context.spot_id ();
        {
            std::lock_guard lock (observations->mutex);
            observations->completion_spots.push_back (current_spot);
            observations->completions.push_back (completion);
            observations->completion_owners.push_back (
              std::string (_context.actor_ref ().node_rid ().value ()));
            hold = observations->completion_to_hold == observations->completions.size ();
            if (observations->completions.size () == 2)
                held_suffix = observations->native_handoff_hold;
        }
        if (held_suffix)
            held_suffix->next_join_terminal.complete (fw::result_t<void>::success ());
        observations->changed.notify_all ();
        observations->join_completed.complete (fw::result_t<void>::success ());
        if (hold)
            co_await observations->release_completion.task ();
        if (observations->fail_completion)
            throw std::runtime_error ("completion callback failed");
        co_return;
    }

  private:
    fw::actor_context_t _context;
};
class factory_t final : public fw::actor_factory_t<actor_t>
{
  public:
    fw::task_t<std::shared_ptr<actor_t>> create (fw::actor_context_t context,
                                                 std::stop_token) override
    {
        if (observations->fail_remote_factory
            && context.actor_ref ().node_rid ().value () == second_rid) {
            {
                std::lock_guard lock (observations->mutex);
                ++observations->remote_factory_failure_count;
            }
            throw std::runtime_error ("target Actor factory failed");
        }
        {
            std::lock_guard lock (observations->mutex);
            observations->factory_nodes.emplace_back (context.actor_ref ().node_rid ().value ());
        }
        co_return std::make_shared<actor_t> (std::move (context));
    }
};
class entry_t final : public fw::entry_spot_t<actor_t>
{
  public:
    explicit entry_t (fw::entry_spot_context_t context) : _context (std::move (context)) {}
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_actor_send<&entry_t::move> ();
        _context.handlers ().add_actor_request<&entry_t::probe> ();
        _context.handlers ().add_actor_send<&entry_t::view> ();
        _context.handlers ().add_actor_send<&entry_t::record> ();
    }
    void record (actor_t &, fw::message_context_t &, const record_t &request)
    {
        {
            std::lock_guard lock (observations->mutex);
            observations->recorded.push_back (request.value);
        }
        observations->changed.notify_all ();
    }
    fw::task_t<void> view (actor_t &actor, fw::message_context_t &, const view_request_t &)
    {
        co_await actor.context ().bound_session ().send (view_t{actor.instance}).async ();
    }
    probe_t probe (actor_t &actor, fw::message_context_t &, const probe_t &request)
    {
        {
            std::lock_guard lock (observations->mutex);
            observations->intake_probe_values.push_back (request.value);
        }
        observations->changed.notify_all ();
        return request.value == instance_probe ? probe_t{actor.instance} : request;
    }
    void move (actor_t &actor, fw::message_context_t &, const move_t &request)
    {
        actor.context ().join_spot (fw::spot_id_t (request.target)).defer ();
    }
    fw::task_t<fw::actor_create_response_t> on_create_actor (actor_t &,
                                                             const fw::message_t &) override
    {
        {
            std::lock_guard lock (observations->mutex);
            ++observations->creation_callbacks;
        }
        if (observations->creation_case == creation_case_t::failed)
            throw std::runtime_error ("Entry creation failed");
        co_return observations->creation_case == creation_case_t::rejected
          ? fw::actor_create_response_t::reject ()
          : fw::actor_create_response_t::accept ();
    }
    fw::task_t<void> on_actor_joined (actor_t &) override
    {
        std::lock_guard lock (observations->mutex);
        ++observations->entry_joined;
        co_return;
    }
    fw::task_t<void> on_leave_actor (actor_t &) override
    {
        std::lock_guard lock (observations->mutex);
        ++observations->entry_left;
        co_return;
    }

  private:
    fw::entry_spot_context_t _context;
};
// A second Actor type, so a node can register an Entry Spot that accepts only it.
constexpr auto other_actor_type = "other-member";
class other_actor_t final : public fw::actor_t
{
  public:
    explicit other_actor_t (fw::actor_context_t context) : _context (std::move (context)) {}
    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }

  private:
    fw::actor_context_t _context;
};
class other_factory_t final : public fw::actor_factory_t<other_actor_t>
{
  public:
    fw::task_t<std::shared_ptr<other_actor_t>> create (fw::actor_context_t context,
                                                       std::stop_token) override
    {
        co_return std::make_shared<other_actor_t> (std::move (context));
    }
};
class other_entry_t final : public fw::entry_spot_t<other_actor_t>
{
  public:
    explicit other_entry_t (fw::entry_spot_context_t context) : _context (std::move (context)) {}
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    fw::task_t<void> on_actor_joined (other_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (other_actor_t &) override { co_return; }

  private:
    fw::entry_spot_context_t _context;
};
class room_t final : public fw::spot_t<actor_t>
{
  public:
    explicit room_t (fw::spot_context_t context) : _context (std::move (context)) {}
    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_actor_send<&room_t::move> ();
        _context.handlers ().add_actor_request<&room_t::probe> ();
        _context.handlers ().add_actor_send<&room_t::push> ();
        _context.handlers ().add_actor_send<&room_t::view> ();
        _context.handlers ().add_handler<&room_t::ready> ();
        _context.handlers ().add_handler<&room_t::ordinary_probe> ();
    }
    fw::task_t<void> view (actor_t &actor, fw::message_context_t &, const view_request_t &)
    {
        co_await actor.context ().bound_session ().send (view_t{actor.instance}).async ();
    }
    fw::task_t<probe_t> ordinary_probe (const probe_t &request)
    {
        const auto observation = observations->ordinary_spot;
        if (observation) {
            if (request.value == 1) {
                observation->entered.complete (fw::result_t<void>::success ());
                co_await observation->release.task ();
            }
            observation->handled.push_back (request.value);
        }
        co_return request;
    }
    fw::task_t<void> ready (const ready_t &)
    {
        _context.relocation_ready ().defer ();
        co_return;
    }
    void move (actor_t &actor, fw::message_context_t &, const move_t &request)
    {
        if (request.target == entry_target) {
            actor.context ().join_entry_spot ().defer ();
            return;
        }
        if (!observations->joined_sender || request.target != current_spot) {
            actor.context ().join_spot (fw::spot_id_t (request.target)).defer ();
            return;
        }
        try {
            actor.context ().join_spot (fw::spot_id_t (request.target)).defer ();
        }
        catch (const std::exception &error) {
            {
                std::lock_guard lock (observations->mutex);
                observations->next_join_error = error.what ();
                observations->next_join_attempted = true;
            }
            observations->changed.notify_all ();
            throw;
        }
        {
            std::lock_guard lock (observations->mutex);
            observations->next_join_attempted = true;
        }
        observations->changed.notify_all ();
    }
    fw::task_t<void> push (actor_t &actor, fw::message_context_t &, const probe_t &message)
    {
        if (!observations->joined_payloads.empty ()) {
            {
                std::lock_guard lock (observations->mutex);
                observations->handled_payloads.push_back (message.value);
                if (observations->transfer_suffix) {
                    observations->handled_spots.emplace_back (_context.spot_id ());
                    observations->handled_nodes.emplace_back (
                      actor.context ().actor_ref ().node_rid ().value ());
                    observations->handled_completion_counts.push_back (
                      observations->completions.size ());
                }
            }
            observations->changed.notify_all ();
            if (message.value < 0)
                throw std::runtime_error ("replayed payload failed");
            if (observations->transfer_suffix && message.value == 1)
                actor.context ().join_spot (fw::spot_id_t (suffix_spot)).defer ();
            co_return;
        }
        co_await actor.context ().bound_session ().send (message).async ();
    }
    probe_t probe (actor_t &actor, fw::message_context_t &, const probe_t &request)
    {
        if (!observations->joined_payloads.empty ()) {
            std::lock_guard lock (observations->mutex);
            observations->handled_payloads.push_back (request.value);
            if (observations->transfer_suffix) {
                observations->handled_spots.emplace_back (_context.spot_id ());
                observations->handled_nodes.emplace_back (
                  actor.context ().actor_ref ().node_rid ().value ());
                observations->handled_completion_counts.push_back (
                  observations->completions.size ());
            }
        }
        observations->changed.notify_all ();
        return request.value == instance_probe ? probe_t{actor.instance} : request;
    }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view joining_actor,
                                                            const fw::message_t &) override
    {
        if (observations->consecutive_join_probe && _context.spot_id () == current_spot) {
            {
                std::lock_guard lock (observations->mutex);
                if (joining_actor == actor_id)
                    observations->first_admission_accepted = true;
                if (joining_actor == "second-member")
                    observations->second_admission_during_capture =
                      observations->first_capture_pending;
            }
            observations->changed.notify_all ();
        }
        if (observations->hold_join_admission) {
            observations->join_admission_entered.complete (fw::result_t<void>::success ());
            co_await observations->release_join_admission.task ();
        }
        if (_context.spot_id () == failed_spot)
            throw std::runtime_error ("target admission failed");
        if (_context.spot_id () == framework_failed_spot)
            throw fw::framework_exception_t (fw::framework_error_kind_t::unavailable,
                                             "target is unavailable");
        if (_context.spot_id () == rejected_spot)
            co_return fw::spot_actor_join_result_t::reject (std::string (rejection_reply));
        co_return _context.spot_id () == maintenance_spot ? fw::spot_actor_join_result_t::reject ()
                                                          : fw::spot_actor_join_result_t::accept ();
    }
    fw::task_t<void> on_actor_joined (actor_t &actor) override
    {
        if (observations->joined_sender && _context.spot_id () == local_spot) {
            {
                std::lock_guard lock (observations->mutex);
                auto *source = &observations->joined_send_captured;
                observations->joined_admission_completion =
                  observations_t::joined_admission_batch_t{
                    std::max<std::size_t> (1, observations->joined_payloads.size ()),
                    [source] { source->complete (fw::result_t<void>::success ()); }};
            }
            if (observations->joined_payloads.empty ()) {
                co_await observations->joined_sender.load ()
                  ->send (actor.context ().actor_id (), move_t{current_spot})
                  .async ();
            } else {
                for (const auto value : observations->joined_payloads) {
                    if (observations->suffix_request && value == 2) {
                        auto reply = std::make_shared<fw::task_t<probe_t>> (
                          observations->joined_sender.load ()
                            ->request (actor.context ().actor_id (), probe_t{value})
                            .timeout (wait_budget)
                            .async<probe_t> ());
                        observations->suffix_reply = [reply] {
                            const auto result = reply->result ();
                            return result ? fw::result_t<int>::success (result.value ().value)
                                          : fw::result_t<int>::failure (result.error_kind (),
                                                                        result.error ()->what ());
                        };
                    } else {
                        co_await observations->joined_sender.load ()
                          ->send (actor.context ().actor_id (), probe_t{value})
                          .async ();
                    }
                }
            }
            co_await observations->joined_send_captured.task ();
        }
        if (observations->joined_sender && _context.spot_id () == current_spot)
            co_await observations->joined_sender.load ()
              ->send (actor.context ().actor_id (), probe_t{1})
              .async ();
        co_return;
    }
    fw::task_t<void> on_closing (const fw::spot_closing_context_t &context,
                                 std::stop_token) override
    {
        std::lock_guard lock (observations->mutex);
        observations->closings.emplace_back (context.reason, observations->destroyed.size ());
        co_return;
    }
    fw::task_t<void> on_leave_actor (actor_t &) override
    {
        if (_context.spot_id () == previous_spot) {
            {
                std::lock_guard lock (observations->mutex);
                observations->leave_entered = true;
            }
            observations->changed.notify_all ();
            co_await observations->release_leave.task ();
        }
        co_return;
    }

  private:
    fw::spot_context_t _context;
};
class room_state_t final : public fw::spot_relocation_adapter_t<room_t>
{
  public:
    fw::task_t<std::vector<std::byte>> capture (room_t &, std::stop_token) override
    {
        EXPECT_TRUE (fw::detail::capture_current_serial_turn ());
        co_return std::vector<std::byte>{std::byte{1}};
    }
    fw::task_t<void> restore (room_t &, std::vector<std::byte>, std::stop_token) override
    {
        co_return;
    }
};
// Release application callbacks before host teardown, including assertion failure.
struct release_callbacks_t
{
    observations_t &e;
    ~release_callbacks_t ()
    {
        {
            std::lock_guard lock (e.mutex);
            e.release_replay_post = true;
            e.release_replay_prefix = true;
        }
        e.changed.notify_all ();
        e.joined_send_captured.complete (fw::result_t<void>::success ());
        e.release_leave.complete (fw::result_t<void>::success ());
        e.release_join_admission.complete (fw::result_t<void>::success ());
        e.release_completion.complete (fw::result_t<void>::success ());
        if (e.native_handoff_hold)
            e.native_handoff_hold->release.complete (fw::result_t<void>::success ());
    }
};
struct host_t
{
    fw::app_t app = fw::app_t::create ();
    std::thread thread;
    ~host_t ()
    {
        app.request_stop ();
        if (thread.joinable ())
            thread.join ();
    }
    void start ()
    {
        thread = std::thread ([this] {
            char name[] = "maintenance-join";
            char *argv[]{name, nullptr};
            app.run (1, argv);
        });
    }
};
// Which Entry Spot a node registers: one accepting `actor_t`, none, or one accepting only
// `other_actor_t` (that node also registers the `other_actor_t` factory).
enum class entry_kind_t
{
    member,
    none,
    other
};
void observe_joined_batch_admission (observations_t &evidence,
                                     const fw::message_flow_event_t &event)
{
    if (event.actor_id != actor_id
        || (event.packet_name != move_t::packet_name && event.packet_name != probe_t::packet_name))
        return;
    const bool native_admitted =
      event.outcome == fw::message_flow_outcome_t::admitted
      && event.detail_stage == "canonical_application.commit_accepted_ingress";
    const bool retained = event.detail_stage == "relay_actor_packet.try_append_backlog"
                          && event.detail_result == "result=appended";
    std::function<void ()> complete;
    {
        std::lock_guard lock (evidence.mutex);
        if (retained)
            ++evidence.backlog_appends;
        if (native_admitted && evidence.joined_admission_completion
            && --evidence.joined_admission_completion->remaining == 0) {
            complete = std::move (evidence.joined_admission_completion->complete);
            evidence.joined_admission_completion.reset ();
        }
    }
    if (complete)
        complete ();
}

void configure (host_t &host,
                const std::string &rid,
                std::shared_ptr<fw::location_store_t> store,
                std::shared_ptr<fw::relocation_store_t> relocations,
                std::chrono::milliseconds follow_duration = default_follow_duration,
                int spot_limit = 0,
                bool application_signaled = false,
                entry_kind_t entry = entry_kind_t::member,
                std::optional<int> placement_weight = std::nullopt)
{
    auto &options = host.app.add_zlink_framework ();
    if (follow_duration != default_follow_duration) {
        options.configure_locations ().route_cache_max_age = 0ms;
        options.set_message_follow_duration (follow_duration);
    }
    host.app.logging ().use_file (rid + ".flow").set_min_level (fw::log_level_t::debug);
    options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
    fw::detail::dispatch_options_access_t::set_observer_for_tests (
      options.configure_dispatch (), [] (const fw::message_flow_event_t &event) {
          observe_joined_batch_admission (*observations, event);
          const auto hold = observations->native_handoff_hold;
          if (hold && event.actor_id == actor_id && event.spot_id == suffix_spot
              && event.outcome == fw::message_flow_outcome_t::sent
              && event.message_kind == fw::dispatch_message_kind_t::actor_request
              && !event.packet_name) {
              hold->next_join_started.complete (fw::result_t<void>::success ());
              observations->changed.notify_all ();
          }
          if (hold && event.actor_id == actor_id && event.packet_name == probe_t::packet_name) {
              bool entered = false;
              if (event.detail_stage == "invoke_erased.enter") {
                  std::lock_guard lock (observations->mutex);
                  entered = ++hold->entries == 2;
              }
              if (event.spot_id == local_spot
                  && event.detail_stage == "invoke_erased.application_handler"
                  && event.detail_result == "result=finished success=true") {
                  hold->preceding_h_finished.complete (fw::result_t<void>::success ());
                  observations->changed.notify_all ();
              }
              if (entered) {
                  hold->entered.complete (fw::result_t<void>::success ());
                  observations->changed.notify_all ();
                  hold->release.task ().result ().value ();
              }
          }
          {
              std::lock_guard lock (observations->mutex);
              if (event.packet_name == probe_t::packet_name && event.actor_id == actor_id
                  && event.message_kind == fw::dispatch_message_kind_t::actor_request
                  && event.outcome == fw::message_flow_outcome_t::admitted
                  && event.detail_stage == "canonical_application.commit_accepted_ingress")
                  ++observations->ordinary_posts;
              if (event.packet_name == "handoff_replay_failed")
                  observations->replay_failures.push_back (event);
          }
          observations->changed.notify_all ();
          if (observations->hold_replay_prefix && event.actor_id == actor_id
              && event.spot_id == local_spot && event.packet_name == probe_t::packet_name
              && event.detail_stage == "invoke_erased.enter") {
              std::unique_lock lock (observations->mutex);
              observations->replay_prefix_entered = true;
              observations->changed.notify_all ();
              observations->changed.wait (lock, [] { return observations->release_replay_prefix; });
          }
          if (!observations->joined_sender || observations->send_after_close
              || event.packet_name != move_t::packet_name || event.spot_id != local_spot
              || event.detail_stage != "invoke_erased.post_serial")
              return;
          std::unique_lock lock (observations->mutex);
          observations->changed.wait (lock, [] { return observations->release_replay_post; });
      });
    options.add_location_store (store);
    options.add_relocation_store (relocations);
    auto mesh = options.add_route_mesh (mesh_name);
    mesh.set_object_role (fw::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from (rid))
      .listen ("tcp://127.0.0.1:0");
    if (placement_weight)
        mesh.set_placement_weight (*placement_weight);
    if (entry == entry_kind_t::member)
        mesh.add_entry_spot<entry_t> ([] (fw::entry_spot_context_t context) {
            return std::make_shared<entry_t> (std::move (context));
        });
    if (entry == entry_kind_t::other)
        mesh
          .add_entry_spot<other_entry_t> ([] (fw::entry_spot_context_t context) {
              return std::make_shared<other_entry_t> (std::move (context));
          })
          .add_actor_factory<other_actor_t, other_factory_t> (
            other_actor_type, std::make_shared<other_factory_t> (),
            [] (auto &factory) { factory.recreate_on_relocation (); });
    mesh
      .add_spot_factory<room_t> (
        room_type,
        [] (fw::spot_context_t context) { return std::make_shared<room_t> (std::move (context)); },
        [spot_limit, application_signaled] (auto &factory) {
            if (application_signaled) {
                factory.set_relocation_coordination_mode (
                  fw::spot_relocation_coordination_mode_t::application_signaled);
                factory.template preserve_state_with<room_state_t> ();
            } else
                factory.disable_relocation ();
            if (spot_limit > 0)
                factory.set_stable_type_limit (spot_limit);
        })
      .add_actor_factory<actor_t, factory_t> (
        actor_type, std::make_shared<factory_t> (),
        [] (auto &factory) { factory.recreate_on_relocation (); });
}
// A Session owner that binds the member Actor on its first packet and relays
// every packet to it, as an application Session does.
class relay_session_t final : public fw::packet_stream_session_t
{
  public:
    explicit relay_session_t (fw::actor_manager_t &actors) : _actors (actors) {}
    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }
    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        auto bound = stream.actors ().bound ();
        if (bound.empty ()) {
            const auto ensured =
              co_await _actors.get_or_create (fw::actor_id_t (actor_id), actor_type)
                .in_mesh (mesh_name)
                .async ();
            const auto ref = std::visit (
              [] (const auto &result) -> fw::actor_ref_t {
                  if constexpr (requires { result.actor; })
                      return result.actor;
                  else
                      throw std::runtime_error ("Actor creation rejected");
              },
              ensured);
            co_await stream.actors ().bind_or_get (ref).async ();
            bound = stream.actors ().bound ();
        }
        co_await bound.front ().relay (std::string (dispatch.packet_name), payload);
    }

  private:
    fw::actor_manager_t &_actors;
};
constexpr auto session_rid = "join-session";
constexpr auto stream_name = "maintenance-join-stream";
void configure_session (host_t &host,
                        std::shared_ptr<fw::location_store_t> store,
                        std::shared_ptr<fw::relocation_store_t> relocations)
{
    auto &options = host.app.add_zlink_framework ();
    options.configure_locations ().route_cache_max_age = 0ms;
    options.set_message_follow_duration (source_follow_duration);
    host.app.logging ()
      .use_file (std::string (session_rid) + ".flow")
      .set_min_level (fw::log_level_t::debug);
    options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
    options.add_location_store (store);
    options.add_relocation_store (relocations);
    options.add_route_mesh (mesh_name)
      .set_object_role (fw::object_role_t::client)
      .set_routing_id (zlink::routing_id_t::from (session_rid))
      .listen ("tcp://127.0.0.1:0");
    options.add_stream_node (stream_name)
      .bind ("tcp://127.0.0.1:0")
      .register_session<relay_session_t> ();
}
// One client connection that keeps every received View in arrival order.
class view_client_t
{
  public:
    explicit view_client_t (std::string endpoint)
    {
        zlink::stream_connector::connector_options_t options;
        options.endpoint = std::move (endpoint);
        options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        options.reconnect.enabled = false;
        _connector = zlink::stream_connector::connector_factory_t::create (options);
        _subscription = _connector.on<view_t> ([this] (const auto &message) {
            {
                std::lock_guard lock (_mutex);
                _views.push_back (message.payload.instance);
            }
            _changed.notify_all ();
        });
    }
    ~view_client_t () { (void) _connector.close (); }
    bool connect () { return static_cast<bool> (_connector.connect ()); }
    // Sends one View request on this connection and returns the View it produced.
    std::optional<int> view ()
    {
        std::unique_lock lock (_mutex);
        const auto before = _views.size ();
        lock.unlock ();
        _connector.send (view_request_t{}).submit ();
        lock.lock ();
        if (!_changed.wait_for (lock, wait_budget, [&] { return _views.size () > before; }))
            return std::nullopt;
        return _views[before];
    }

  private:
    zlink::stream_connector::connector_t _connector;
    zlink::stream_connector::subscription_t _subscription;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<int> _views;
};
bool ready (host_t &host)
{
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!host.app.is_ready () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    return host.app.is_ready ();
}
bool move_to (fw::actor_client_t &sender,
              observations_t &evidence,
              std::string target,
              std::size_t count)
{
    auto sent =
      sender.send (fw::actor_id_t (actor_id), move_t{std::move (target)}).async ().result ();
    if (!sent) {
        std::fprintf (stderr, "maintenance move send failed: kind=%d error=%s\n",
                      static_cast<int> (sent.error_kind ()),
                      sent.error () ? sent.error ()->what () : "no error");
        return false;
    }
    std::unique_lock lock (evidence.mutex);
    return evidence.changed.wait_for (lock, wait_budget,
                                      [&] { return evidence.completions.size () >= count; });
}
bool peer_ready (fw::route_mesh_runtime_t &routes,
                 std::string_view peer_rid = {},
                 std::size_t count = 1)
{
    const auto ready = [&] {
        const auto snapshot = routes.snapshot (mesh_name);
        return snapshot.ready_peer_count >= count
               && std::any_of (
                 snapshot.peers.begin (), snapshot.peers.end (), [&] (const auto &peer) {
                     return peer.state == fw::peer_state_t::ready
                            && (peer_rid.empty () || peer.node_rid.to_string () == peer_rid);
                 });
    };
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!ready () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    return ready ();
}

void joined_actor_chain (bool send_after_close)
{
    observations_t evidence;
    evidence.send_after_close = send_after_close;
    observations = &evidence;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, first_rid, std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    auto &client = services.get_required<fw::actor_client_t> ();
    if (!send_after_close)
        evidence.joined_sender = &client;
    auto &spots = services.get_required<fw::spot_manager_t> ();
    for (const auto *id : {local_spot, current_spot})
        ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (id), room_type).async ().result ());
    const auto created = services.get_required<fw::actor_manager_t> ()
                           .get_or_create (fw::actor_id_t (actor_id), actor_type)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    const auto actor = std::get<fw::actor_create_created_t> (created.value ()).actor;
    ASSERT_TRUE (services.get_required<fw::detail::actor_gateway_runtime_t> ().bind_session_sink (
      actor, [&] (std::string, fw::stream_codec_t, const zlink::message_t &) -> fw::task_t<void> {
          {
              std::lock_guard lock (evidence.mutex);
              ++evidence.session_pushes;
          }
          evidence.changed.notify_all ();
          co_return;
      }));
    std::shared_ptr<fw::runtime::serial_execution_queue_t> source_queue;
    if (send_after_close) {
        const auto state =
          fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
            .front ()
            ->spot_state;
        const auto key = std::string (actor_type) + ":" + actor_id;
        const auto context =
          state->lane
            .run ([&] {
                return fw::detail::spot_context_access_t::state (
                  state->spot_contexts_by_id.at (std::string (state->actor_spot_ids.at (key))));
            })
            .get ();
        source_queue = context->ensure_spot_serial_executor ()->actor_executor (key)->queue ();
    }
    ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), move_t{local_spot}).async ().result ());
    if (send_after_close) {
        std::unique_lock entered (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (entered, wait_budget,
                                                [&] { return !evidence.completions.empty (); }));
        entered.unlock ();
        const auto state =
          fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
            .front ()
            ->spot_state;
        const auto key = std::string (actor_type) + ":" + actor_id;
        const auto deadline = std::chrono::steady_clock::now () + wait_budget;
        while (state->actor_transfer_coordinator->phase (key)
               && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        const bool closed = !state->actor_transfer_coordinator->phase (key);
        ASSERT_TRUE (closed);
        source_queue->drain ();
        evidence.joined_sender = &client;
        ASSERT_TRUE (
          client.send (fw::actor_id_t (actor_id), move_t{current_spot}).async ().result ());
    }
    std::unique_lock lock (evidence.mutex);
    const auto attempted =
      evidence.changed.wait_for (lock, wait_budget, [&] { return evidence.next_join_attempted; });
    evidence.release_replay_post = true;
    evidence.changed.notify_all ();
    ASSERT_TRUE (attempted);
    ASSERT_TRUE (evidence.next_join_error.empty ())
      << evidence.next_join_error << " completions=" << evidence.completions.size ()
      << " pushes=" << evidence.session_pushes;
    ASSERT_TRUE (evidence.changed.wait_for (
      lock, wait_budget,
      [&] { return evidence.completions.size () >= 2 && evidence.session_pushes == 1; }))
      << "completions=" << evidence.completions.size () << " pushes=" << evidence.session_pushes;
    for (const auto &completion : evidence.completions)
        EXPECT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (completion));
}

class consecutive_join_state_t final : public fw::actor_relocation_adapter_t<actor_t>
{
  public:
    fw::task_t<std::vector<std::byte>> capture (actor_t &actor, std::stop_token) override
    {
        if (actor.context ().actor_id ().value () == actor_id
            && actor.context ().actor_ref ().node_rid ().value () == first_rid) {
            {
                std::lock_guard lock (observations->mutex);
                EXPECT_TRUE (observations->first_admission_accepted);
                observations->first_capture_pending = true;
            }
            observations->changed.notify_all ();
            co_await observations->release_completion.task ();
            {
                std::lock_guard lock (observations->mutex);
                observations->first_capture_pending = false;
            }
        }
        co_return std::vector<std::byte>{std::byte{1}};
    }
    fw::task_t<void> restore (actor_t &, std::vector<std::byte>, std::stop_token) override
    {
        co_return;
    }
};

TEST (ActorMaintenanceJoin, ConsecutiveDeferredJoinsDoNotHoldTargetSpotDuringCapture)
{
    observations_t evidence;
    evidence.consecutive_join_probe = true;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, other_source, target;
    release_callbacks_t release{evidence};
    // Target admission accepts P1 before its source Capture waits. The
    // target Host and its Spot callback gate remain available for P2.
    const auto configure_public = [&] (host_t &host, const char *rid) {
        auto &options = host.app.add_zlink_framework ();
        options.add_location_store (store);
        options.add_relocation_store (relocations);
        host.app.logging ().use_file (std::string (rid) + ".flow");
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
        auto mesh = options.add_route_mesh (mesh_name);
        mesh.set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (rid))
          .listen ("tcp://127.0.0.1:0");
        mesh.add_entry_spot<entry_t> ([] (fw::entry_spot_context_t context) {
            return std::make_shared<entry_t> (std::move (context));
        });
        mesh
          .add_spot_factory<room_t> (
            room_type,
            [] (fw::spot_context_t context) {
                return std::make_shared<room_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_actor_factory<actor_t, factory_t> (
            actor_type, std::make_shared<factory_t> (), [] (auto &factory) {
                factory.template preserve_state_with<consecutive_join_state_t> ();
            });
    };
    configure_public (source, first_rid);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    auto &actors = services.get_required<fw::actor_manager_t> ();
    ASSERT_TRUE (actors.get_or_create (fw::actor_id_t (actor_id), actor_type).async ().result ());
    services.get_required<fw::route_mesh_runtime_options_t> ().mesh (mesh_name).placement_weight (
      0);
    // Source Capture may occupy P1's Host. P2 has an independent source
    // so this test isolates the target Spot FIFO.
    configure_public (other_source, third_rid);
    other_source.start ();
    ASSERT_TRUE (ready (other_source));
    auto other_services = other_source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (other_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t ("second-member"), actor_type)
                   .async ()
                   .result ());
    other_services.get_required<fw::route_mesh_runtime_options_t> ()
      .mesh (mesh_name)
      .placement_weight (0);
    configure_public (target, second_rid);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    ASSERT_TRUE (peer_ready (other_services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    auto target_services = target.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (target_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), move_t{current_spot}).async ().result ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.first_capture_pending; }));
    }
    ASSERT_TRUE (other_services.get_required<fw::actor_client_t> ()
                   .send (fw::actor_id_t ("second-member"), move_t{current_spot})
                   .async ()
                   .result ());
    {
        std::unique_lock lock (evidence.mutex);
        // The watchdog bounds failure cleanup; callback signals define ordering.
        const bool admitted = evidence.changed.wait_for (
          lock, wait_budget, [&] { return evidence.second_admission_during_capture; });
        EXPECT_TRUE (admitted);
        EXPECT_TRUE (evidence.first_capture_pending);
    }
    evidence.release_completion.complete (fw::result_t<void>::success ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.completions.size () == 2; }));
        for (const auto &completion : evidence.completions) {
            const auto *failed = std::get_if<fw::actor_join_failed_t> (&completion);
            EXPECT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (completion))
              << "completion_variant=" << completion.index ()
              << " failed_kind=" << (failed ? static_cast<int> (failed->error_kind) : -1);
        }
    }
}

TEST (ActorMaintenanceJoin, JoinedActorSendCanDeferNextJoin)
{
    joined_actor_chain (false);
}
TEST (ActorMaintenanceJoin, AssignmentAfterMoveClosesReachesSession)
{
    joined_actor_chain (true);
}

enum class backlog_case_t
{
    ordinary_order,
    completion_failure,
    replay_failure
};
void local_join_backlog (backlog_case_t scenario)
{
    observations_t evidence;
    evidence.joined_payloads =
      scenario == backlog_case_t::replay_failure ? std::vector<int>{-1} : std::vector<int>{1, 2};
    evidence.fail_completion = scenario == backlog_case_t::completion_failure;
    observations = &evidence;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, first_rid, std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    auto &client = services.get_required<fw::actor_client_t> ();
    evidence.joined_sender = &client;
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    const auto ingress_ready = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                                 .async<probe_t> ()
                                 .result ();
    ASSERT_TRUE (ingress_ready);
    evidence.hold_replay_prefix = scenario == backlog_case_t::ordinary_order;
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto key = std::string (actor_type) + ":" + actor_id;
    auto actor_queue = [&] (fw::spot_id_t spot) {
        auto context = state->lane
                         .run ([&] {
                             return fw::detail::spot_context_access_t::state (
                               state->spot_contexts_by_id.at (std::string (spot)));
                         })
                         .get ();
        return context->ensure_spot_serial_executor ()->actor_executor (key)->queue ();
    };
    const auto source = state->lane.run ([&] { return state->actor_spot_ids.at (key); }).get ();
    auto source_queue = actor_queue (source);
    ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), move_t{local_spot}).async ().result ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return !evidence.completions.empty (); }));
    }
    if (scenario == backlog_case_t::ordinary_order) {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.replay_prefix_entered; }));
    } else {
        source_queue->drain ();
    }
    if (scenario == backlog_case_t::completion_failure) {
        auto sealed_queue = actor_queue (fw::spot_id_t (local_spot));
        ASSERT_TRUE (sealed_queue);
        (void) sealed_queue->pending_messages ().result ().value ();
        EXPECT_EQ (state->actor_transfer_coordinator->phase (key),
                   fw::detail::actor_move_phase_t::local);
        {
            std::lock_guard lock (evidence.mutex);
            // This later one-record batch owns its own admission future.
            evidence.joined_send_captured = fw::task_completion_source_t<void>{};
            auto *source = &evidence.joined_send_captured;
            evidence.joined_admission_completion = observations_t::joined_admission_batch_t{
              1, [source] { source->complete (fw::result_t<void>::success ()); }};
        }
        ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), probe_t{3}).async ().result ());
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.backlog_appends == 3; }));
        EXPECT_TRUE (evidence.joined_send_captured.task ().await_ready ());
        EXPECT_TRUE (evidence.handled_payloads.empty ());
        EXPECT_EQ (evidence.completions.size (), 1u);
        EXPECT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.front ()));
        lock.unlock ();
        // The queue control future observes published work while keeping the
        // host executor available for the later canonical ingress.
        (void) sealed_queue->pending_messages ().result ().value ();
        const auto actor = state->lane
                             .run ([&] {
                                 return fw::detail::actor_ref_access_t::make (
                                   fw::node_rid_t::from_string (first_rid), actor_type, actor_id,
                                   state->actor_generations.at (key));
                             })
                             .get ();
        fw::detail::spot_node_runtime_t runtime (state);
        runtime.seal_actor_application_suffix (actor, {}).result ().value ();
        auto backlog = state->actor_transfer_coordinator->cancel_move (key);
        EXPECT_EQ (backlog.size (), 3u);
        std::vector<int> durable_payloads;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> durable_operations;
        for (const auto &packet : backlog) {
            ASSERT_TRUE (packet.application_record);
            ASSERT_TRUE (packet.application_record->payload);
            EXPECT_TRUE (packet.application_record->saved_work ());
            const auto &turn = *packet.application_record->payload;
            ASSERT_TRUE (turn.application_record || turn.frozen_record);
            const auto canonical = turn.application_record
                                     ? fw::runtime::protocol::summarize_frozen_application_record (
                                         *turn.application_record)
                                     : *turn.frozen_record;
            EXPECT_EQ (canonical.kind, fw::runtime::protocol::frozen_record_kind_t::actor_send);
            EXPECT_EQ (canonical.source.node_routing_id,
                       zlink::routing_id_t::from (first_rid).to_bytes ());
            EXPECT_TRUE (canonical.operation.high != 0 || canonical.operation.low != 0);
            durable_operations.emplace_back (canonical.operation.high, canonical.operation.low);
            fw::runtime::stateful::with_accepted_application (
              turn, [&] (auto, const auto &payload) {
                  fw::runtime::messaging::message_parts_t parts (
                    fw::runtime::protocol::decode_application_parts (payload));
                  const auto body = fw::runtime::messaging::envelope_codec_t{}.decode_body (parts);
                  ASSERT_TRUE (body);
                  durable_payloads.push_back (
                    nlohmann::json::parse (body.value ().to_string ()).get<probe_t> ().value);
              });
        }
        std::sort (durable_payloads.begin (), durable_payloads.end ());
        EXPECT_EQ (durable_payloads, (std::vector<int>{1, 2, 3}));
        std::sort (durable_operations.begin (), durable_operations.end ());
        EXPECT_EQ (std::adjacent_find (durable_operations.begin (), durable_operations.end ()),
                   durable_operations.end ());
        EXPECT_TRUE (runtime.take_actor_application_suffix (actor, {}).result ().value ().empty ());
        {
            std::lock_guard guard (evidence.mutex);
            EXPECT_TRUE (evidence.handled_payloads.empty ());
        }
        return;
    }
    if (scenario == backlog_case_t::ordinary_order) {
        EXPECT_FALSE (state->actor_transfer_coordinator->phase (key));
        auto ordinary = client.request (fw::actor_id_t (actor_id), probe_t{3}).async<probe_t> ();
        {
            std::unique_lock lock (evidence.mutex);
            ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                    [&] { return evidence.ordinary_posts > 0; }));
            EXPECT_TRUE (evidence.handled_payloads.empty ());
        }
        {
            std::lock_guard lock (evidence.mutex);
            evidence.release_replay_prefix = true;
        }
        evidence.changed.notify_all ();
        const auto replied = ordinary.result ();
        ASSERT_TRUE (replied);
        EXPECT_EQ (replied.value ().value, 3);
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (
          lock, wait_budget, [&] { return evidence.handled_payloads.size () == 3; }));
        EXPECT_EQ (evidence.handled_payloads, (std::vector<int>{1, 2, 3}));
    } else {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (
          lock, wait_budget, [&] { return !evidence.replay_failures.empty (); }));
        EXPECT_FALSE (state->actor_transfer_coordinator->phase (key));
        const auto &event = evidence.replay_failures.front ();
        EXPECT_EQ (event.result, fw::message_flow_result_t::failed);
        ASSERT_TRUE (event.exception);
        EXPECT_EQ (event.flow_id, event.correlation_id);
        EXPECT_FALSE (event.flow_id.value_or ("").empty ());
        try {
            std::rethrow_exception (event.exception);
        }
        catch (const std::exception &error) {
            EXPECT_STREQ (error.what (), "replayed payload failed");
        }
    }
}
TEST (ActorMaintenanceJoin, LocalJoinPlaceholderKeepsOrdinaryIngressBehindBacklog)
{
    local_join_backlog (backlog_case_t::ordinary_order);
}
TEST (ActorMaintenanceJoin, LocalJoinCompletionFailureKeepsBacklogSealed)
{
    local_join_backlog (backlog_case_t::completion_failure);
}
TEST (ActorMaintenanceJoin, ReplayFailureHasFailedOutcomeAndException)
{
    local_join_backlog (backlog_case_t::replay_failure);
}

std::string suffix_failure_snapshot (host_t &host)
{
    namespace st = fw::runtime::stateful;
    const auto registration =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ()).front ();
    const auto node = fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
    std::string result;
    for (const auto &item : node->objects ().inventory ()) {
        if (item.owner.kind == st::object_kind_t::actor && item.owner.key == actor_id) {
            result += " kernel_state=" + std::to_string (static_cast<int> (item.state))
                      + " kernel_pending="
                      + std::to_string (
                        node->objects ().pending (item.owner, st::turn_domain_t::application));
        }
    }
    result += " native_pending="
              + std::to_string (node->transport ().mailbox ().pending_messages (
                fw::runtime::mesh::service_mailbox_domain_t::application));
    return result;
}

void next_join_suffix (bool remote,
                       bool request = false,
                       bool restore_source = false,
                       bool native_initiator = false,
                       bool hold_native_suffix = false)
{
    observations_t evidence;
    evidence.joined_payloads = {1, 2};
    evidence.transfer_suffix = true;
    evidence.suffix_request = request;
    evidence.fail_remote_factory = restore_source;
    if (hold_native_suffix)
        evidence.native_handoff_hold = std::make_shared<native_handoff_hold_t> ();
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target, initiator;
    release_callbacks_t release{evidence};
    configure (source, first_rid, store, relocations);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (local_spot), room_type).async ().result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    if (remote) {
        configure (target, second_rid, store, relocations);
        target.start ();
        ASSERT_TRUE (ready (target));
        ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
        services.get_required<fw::route_mesh_runtime_options_t> ()
          .mesh (mesh_name)
          .placement_weight (0);
        auto target_services = target.app.advanced ().services ().build_provider ();
        ASSERT_TRUE (target_services.get_required<fw::spot_manager_t> ()
                       .get_or_create (fw::spot_id_t (suffix_spot), room_type)
                       .async ()
                       .result ());
    } else {
        ASSERT_TRUE (
          spots.get_or_create (fw::spot_id_t (suffix_spot), room_type).async ().result ());
    }
    auto &sender = services.get_required<fw::actor_client_t> ();
    evidence.joined_sender = &sender;
    fw::actor_client_t *initial_sender = &sender;
    std::optional<fw::service_provider_t> initiator_services;
    if (native_initiator) {
        configure (initiator, third_rid, store, relocations);
        initiator.start ();
        ASSERT_TRUE (ready (initiator));
        ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), third_rid));
        initiator_services.emplace (initiator.app.advanced ().services ().build_provider ());
        initial_sender = &initiator_services->get_required<fw::actor_client_t> ();
    }
    ASSERT_TRUE (
      initial_sender->send (fw::actor_id_t (actor_id), move_t{local_spot}).async ().result ());
    if (const auto hold = evidence.native_handoff_hold) {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget, [&] {
            return hold->entered.task ().await_ready ()
                   && hold->preceding_h_finished.task ().await_ready ()
                   && hold->next_join_started.task ().await_ready ()
                   && hold->next_join_terminal.task ().await_ready ();
        }));
        EXPECT_EQ (evidence.handled_payloads, (std::vector<int>{1}));
        EXPECT_EQ (evidence.handled_completion_counts, (std::vector<std::size_t>{1}));
        EXPECT_FALSE (hold->release.task ().await_ready ());
        const auto registration = fw::detail::mesh_node_runtime_t::registrations (
                                    (remote ? target : source).app.advanced ().zlink ())
                                    .front ();
        const auto node = fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
        const auto owner =
          node->objects ().find (fw::runtime::stateful::object_kind_t::actor, actor_id);
        EXPECT_TRUE (owner);
        if (owner)
            EXPECT_EQ (
              node->objects ().pending (*owner, fw::runtime::stateful::turn_domain_t::application),
              0u);
        const auto *failed = std::get_if<fw::actor_join_failed_t> (&evidence.completions.back ());
        std::printf ("held-join-terminal-before-release type=%zu error_kind=%d release_ready=%d "
                     "completions=%zu\n",
                     evidence.completions.back ().index (),
                     failed ? static_cast<int> (failed->error_kind) : -1,
                     hold->release.task ().await_ready (), evidence.completions.size ());
        if (!remote && owner) {
            const auto target_spot =
              node->objects ().find (fw::runtime::stateful::object_kind_t::user_spot, suffix_spot);
            EXPECT_TRUE (target_spot);
            if (target_spot) {
                auto [prepare, token] =
                  node->objects ().begin_membership_move (*owner, *target_spot);
                std::printf ("held-kernel-membership-before-release error=%d expected_ready=%d "
                             "release_ready=%d actor=%s generation=%llu authority=%llu\n",
                             static_cast<int> (prepare),
                             static_cast<int> (fw::runtime::stateful::stateful_error_t::none),
                             hold->release.task ().await_ready (), owner->key.c_str (),
                             static_cast<unsigned long long> (owner->object_generation),
                             static_cast<unsigned long long> (owner->authority_owner_generation));
                EXPECT_EQ (prepare, fw::runtime::stateful::stateful_error_t::none);
                if (prepare == fw::runtime::stateful::stateful_error_t::none)
                    EXPECT_EQ (node->objects ().abort_membership_move (token),
                               fw::runtime::stateful::stateful_error_t::none);
            }
        }
        std::printf (
          "native-publication-hold entries=%d Hdone=%d next_join_started=%d handlers=%zu%s\n",
          hold->entries, hold->preceding_h_finished.task ().await_ready (),
          hold->next_join_started.task ().await_ready (), evidence.handled_payloads.size (),
          suffix_failure_snapshot (source).c_str ());
        lock.unlock ();
        hold->release.complete (fw::result_t<void>::success ());
    }
    std::unique_lock lock (evidence.mutex);
    ASSERT_TRUE (evidence.changed.wait_for (
      lock, wait_budget,
      [&] { return evidence.completions.size () >= 2 && evidence.handled_payloads.size () >= 2; }))
      << "completions=" << evidence.completions.size ()
      << " handled=" << evidence.handled_payloads.size () << suffix_failure_snapshot (source);
    EXPECT_EQ (evidence.handled_payloads, (std::vector<int>{1, 2}));
    EXPECT_EQ (evidence.handled_spots,
               (std::vector<std::string>{local_spot, restore_source ? local_spot : suffix_spot}));
    EXPECT_EQ (
      evidence.handled_nodes,
      (std::vector<std::string>{first_rid, remote && !restore_source ? second_rid : first_rid}));
    EXPECT_EQ (evidence.handled_completion_counts, (std::vector<std::size_t>{1, 2}));
    ASSERT_EQ (evidence.completions.size (), 2u);
    EXPECT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.front ()));
    if (restore_source) {
        EXPECT_FALSE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.back ()));
        EXPECT_EQ (evidence.remote_factory_failure_count, 1);
    } else
        EXPECT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.back ()));
    const auto reply = evidence.suffix_reply;
    lock.unlock ();
    if (request) {
        ASSERT_TRUE (reply);
        const auto result = reply ();
        ASSERT_TRUE (result) << (result.error () ? result.error ()->what () : "no error");
        EXPECT_EQ (result.value (), 2);
    }
}
TEST (ActorMaintenanceJoin, NextLocalJoinTakesUnstartedSuffixOnce)
{
    next_join_suffix (false);
}
TEST (ActorMaintenanceJoin, NextRemoteJoinTakesUnstartedSuffixOnce)
{
    next_join_suffix (true);
}

TEST (ActorMaintenanceJoin, NextLocalJoinPreservesSuffixRequestReply)
{
    next_join_suffix (false, true);
}
TEST (ActorMaintenanceJoin, NextRemoteJoinPreservesSuffixRequestReply)
{
    next_join_suffix (true, true);
}

TEST (ActorMaintenanceJoin, FailedRemoteJoinRestoresUnstartedSuffixOnce)
{
    next_join_suffix (true, false, true);
}

TEST (ActorMaintenanceJoin, FailedRemoteJoinRestoresUnstartedSuffixRequestReply)
{
    next_join_suffix (true, true, true);
}

TEST (ActorMaintenanceJoin, NativeInitialLocalJoinTakesUnstartedSuffixOnce)
{
    next_join_suffix (false, false, false, true);
}
TEST (ActorMaintenanceJoin, NativeInitialRemoteJoinTakesUnstartedSuffixOnce)
{
    next_join_suffix (true, false, false, true);
}

struct native_intake_fixture_t
{
    static constexpr auto source_id = "boundary-source";
    std::vector<std::uint8_t> rid = zlink::routing_id_t::from ("intake-boundary").to_bytes ();
    fw::runtime::stateful::accepted_record_authority_resolver_t resolver;
    std::shared_ptr<fw::runtime::host::public_host_runtime_t> host;
    explicit native_intake_fixture_t (fw::dispatch_options_t dispatch = {})
    {
        fw::runtime::host::host_options_t options;
        options.mesh.descriptor = {mesh_name,
                                   rid,
                                   1,
                                   1,
                                   "tcp://127.0.0.1:0",
                                   {},
                                   fw::runtime::mesh::service_node_state_t::preparing};
        options.mesh.runtime_failures =
          std::make_shared<fw::runtime::runtime_failure_collector_t> ();
        options.mesh.dispatch = std::move (dispatch);
        options.object_stable_types = {actor_type, "framework.spot"};
        host = std::make_shared<fw::runtime::host::public_host_runtime_t> (std::move (options));
        const auto lease = std::make_shared<const fw::location_owner_token_t> (
          fw::location_owner_token_t{source_id, 1});
        resolver = [host = host,
                    lease] (fw::runtime::stateful::accepted_record_authority_query_t query) {
            return host->accepted_record_authority (std::move (query), lease);
        };
        host->configure_stateful_dispatch ([lease] { return lease; });
        auto peer = host->transport ().topology ().local_descriptor ();
        peer.node_routing_id = zlink::routing_id_t::from (source_id).to_bytes ();
        peer.advertised_endpoint = "tcp://127.0.0.1:1";
        auto original = std::make_shared<const fw::mesh_node_descriptor_t> (
          fw::mesh_node_descriptor_t{.mesh_name = peer.mesh_name,
                                     .rid = zlink::routing_id_t::from (peer.node_routing_id),
                                     .lifecycle_generation = peer.lifecycle_generation,
                                     .endpoint = peer.advertised_endpoint,
                                     .owner_id = source_id,
                                     .lease_generation = 1});
        host->transport ().expect_peer (peer, original);
        EXPECT_EQ (host->transport ().topology ().admit (peer, {1}),
                   fw::runtime::mesh::peer_admission_result_t::admitted);
        host->start ();
    }
    fw::runtime::mesh::service_mailbox_record_t
    actor_record (std::uint64_t sequence, std::uint64_t authority_generation = 1) const
    {
        namespace wire = fw::runtime::protocol;
        namespace mesh = fw::runtime::mesh;
        mesh::service_mailbox_record_t record;
        record.owner = mesh::service_mailbox_t::application_owner (
          fw::runtime::host::owner_kind_t::actor, actor_id);
        record.domain = mesh::service_mailbox_domain_t::application;
        record.source_routing_id = zlink::routing_id_t::from (source_id).to_bytes ();
        record.source_node_generation = 1;
        const wire::actor_route_fence_t fence{actor_id, 1, rid, 1, authority_generation, 1};
        wire::application_payload_t::multipart_t parts;
        fw::runtime::messaging::envelope_header_t envelope;
        envelope.kind = fw::runtime::messaging::message_kind_t::command;
        envelope.channel_name = mesh_name;
        envelope.message_name = probe_t::packet_name;
        parts.push_back (fw::runtime::messaging::envelope_codec_t{}.encode_header (envelope));
        record.parts = fw::detail::backend::materialize_binding_parts (
          {wire::encode_actor_message_header (wire::command::actorSend, std::nullopt, fence,
                                              {1, sequence}),
           wire::encode_application_payload (
             wire::application_payload_t::from_parts (std::move (parts)))});
        return record;
    }
};

TEST (ActorMaintenanceJoin, MaintenanceSealPreservesLocalAndRemoteClaimErrorKinds)
{
    for (const bool remote : {false, true}) {
        SCOPED_TRACE (remote ? "remote" : "local");
        fw::detail::actor_transfer_coordinator_t transfers;
        const auto begin = [&] (const std::string &key) {
            return remote ? transfers.try_begin_source_remote (key)
                          : transfers.try_begin_local (key);
        };
        ASSERT_TRUE (begin ("existing"));
        const auto conflict = begin ("existing");
        EXPECT_FALSE (conflict);
        EXPECT_EQ (fw::framework_error_kind_t::rejected, conflict.error_kind ());
        transfers.cancel_move ("existing");
        ASSERT_TRUE (transfers.try_seal_maintenance_inventory ().result ().value ());
        const auto sealed = begin ("late");
        EXPECT_FALSE (sealed);
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, sealed.error_kind ());
        transfers.end_maintenance_inventory ().result ().value ();
        ASSERT_TRUE (transfers.try_reserve_source ("prior"));
        auto waiting = transfers.try_seal_maintenance_inventory ();
        EXPECT_FALSE (waiting.await_ready ());
        EXPECT_TRUE (begin ("prior"));
        transfers.cancel_move ("prior");
        ASSERT_TRUE (waiting.result ().value ());
        transfers.end_maintenance_inventory ().result ().value ();
    }
}

TEST (NativeIntake, CoordinatorAdmissionReturnsNativeClaimWithoutLaneReentry)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    native_intake_fixture_t fixture;
    auto &objects = fixture.host->objects ();
    const st::object_ref_t object{st::object_kind_t::actor,
                                  actor_id,
                                  1,
                                  1,
                                  mesh_name,
                                  zlink::routing_id_t::from (fixture.rid).to_string ()};
    const auto created = objects.begin_reserved_object (object, actor_type, {});
    ASSERT_EQ (st::stateful_error_t::none, created.error);
    ASSERT_EQ (st::stateful_error_t::none, objects.commit_create (created.attempt));
    st::raw_stateful_dispatch_t raw (objects, fixture.host->transport (), fixture.resolver);
    ASSERT_EQ (st::stateful_error_t::none,
               raw
                 .ingest_claimed (object, fixture.actor_record (1), std::function<void ()> ([] {}),
                                  fw::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ());
    auto [claim_error, delivery] = raw.try_claim (object);
    ASSERT_EQ (st::stateful_error_t::none, claim_error);
    ASSERT_TRUE (delivery);
    fw::test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (worker);
    auto record = std::make_shared<rt::serial_application_record_t> ();
    record->transferred = [&] {
        EXPECT_EQ (st::stateful_error_t::none,
                   raw.transfer_application_claim (object, delivery->turn.sequence));
    };
    rt::serial_work_options_t options;
    options.application_record = record;
    EXPECT_NO_THROW ({
        const auto admitted = objects.actor_transfers ()->admit_dispatch_packet (
          "member:member", nullptr, true, {},
          [&] { return queue->try_post ("native-claim", [] {}, options); });
        EXPECT_EQ (std::optional<bool> (true), admitted.queue_submission);
    });
    // On the red input the callback threw before releasing the stateful claim.
    (void) objects.complete_claim (object, st::turn_domain_t::application);
    EXPECT_TRUE (queue->try_post ("after-native-claim", [] {}));
    worker.run_all ();
    queue->close ();
    worker.run_all ();
    queue->drain ();
    worker.retire ();
    fixture.host->close ();
}

TEST (ActorMaintenanceJoin, MaintenanceSealRejectsClaimAfterPriorSourceCompletion)
{
    native_intake_fixture_t fixture;
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (mesh_name);
    fw::detail::spot_node_runtime_t (state).attach_native_node (fixture.host);
    const std::string key = "member:prior";
    ASSERT_TRUE (state->actor_transfer_coordinator->try_reserve_source (key));
    auto completion = state->actor_transfer_coordinator->wait_for_source_completion (key);
    EXPECT_FALSE (completion.await_ready ());
    state->actor_transfer_coordinator->cancel_move (key);
    ASSERT_TRUE (completion.result ());
    auto inventory = fixture.host->objects ().try_begin_maintenance_inventory ();
    ASSERT_TRUE (inventory.result ().value ());
    const auto late = state->actor_transfer_coordinator->try_reserve_source ("member:late");
    EXPECT_FALSE (late);
    EXPECT_EQ (fw::framework_error_kind_t::unavailable, late.error_kind ());
    state->actor_transfer_coordinator->cancel_move ("member:late");
    fixture.host->objects ().end_maintenance_inventory ().result ().value ();
    fixture.host->close ();
}

void claimed_join_native_admission (bool remote)
{
    namespace sf = fw::runtime::stateful;
    native_intake_fixture_t fixture;
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (mesh_name);
    fw::detail::spot_node_runtime_t (state).attach_native_node (fixture.host);
    auto &objects = fixture.host->objects ();
    objects.replace_placement_candidates (
      {{mesh_name, "intake-boundary", {actor_type}, 100, 16, 0, 4, 0}});
    const auto created = objects.begin_create (
      {sf::object_kind_t::actor, actor_id, actor_type, std::string (mesh_name), {}, false, false});
    ASSERT_EQ (sf::create_status_t::reserved, created.status);
    ASSERT_EQ (sf::stateful_error_t::none, objects.commit_create (created.attempt));
    const std::string key = "member:member";
    ASSERT_TRUE (state->actor_transfer_coordinator->try_reserve_source (key));
    if (remote)
        ASSERT_TRUE (state->actor_transfer_coordinator->try_begin_source_remote (key));
    else
        ASSERT_TRUE (state->actor_transfer_coordinator->try_begin_local (key));
    const auto spot = objects.begin_create ({sf::object_kind_t::user_spot,
                                             "local-target",
                                             actor_type,
                                             std::string (mesh_name),
                                             {},
                                             false,
                                             false});
    ASSERT_EQ (sf::create_status_t::reserved, spot.status);
    ASSERT_EQ (sf::stateful_error_t::none, objects.commit_create (spot.attempt));
    auto inventory = objects.try_begin_maintenance_inventory ();
    const sf::object_ref_t target{
      sf::object_kind_t::user_spot, "target", 1, 1, mesh_name, "remote"};
    const auto [error, token] = remote
                                  ? objects.begin_remote_membership_move (created.object, target)
                                  : objects.begin_membership_move (created.object, spot.object);
    EXPECT_EQ (sf::stateful_error_t::none, error);
    EXPECT_FALSE (inventory.await_ready ());
    const auto late = state->actor_transfer_coordinator->try_reserve_source ("member:late");
    EXPECT_FALSE (late);
    EXPECT_EQ (fw::framework_error_kind_t::unavailable, late.error_kind ());
    if (error == sf::stateful_error_t::none)
        objects.abort_membership_move (token);
    state->actor_transfer_coordinator->cancel_move (key);
    state->actor_transfer_coordinator->cancel_move ("member:late");
    ASSERT_TRUE (inventory.result ().value ());
    objects.end_maintenance_inventory ().result ().value ();
    fixture.host->close ();
}
TEST (ActorMaintenanceJoin, ClaimedJoinKeepsNativeAdmissionWhileMaintenanceSeals)
{
    claimed_join_native_admission (true);
}

TEST (ActorMaintenanceJoin, ClaimedLocalJoinKeepsNativeAdmissionWhileMaintenanceSeals)
{
    claimed_join_native_admission (false);
}

TEST (ActorMaintenanceJoin, ObjectSealCapturesChangesAfterJoinSealAndRejectsLaterChanges)
{
    namespace st = fw::runtime::stateful;
    st::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{mesh_name, first_rid, {actor_type}, 100, 16, 0, 16, 0}});
    const auto create = [&] (st::object_kind_t kind, const std::string &key) {
        return objects.begin_create (
          {kind, key, actor_type, std::string (mesh_name), {}, false, false});
    };
    const auto old = create (st::object_kind_t::actor, "old");
    ASSERT_EQ (st::create_status_t::reserved, old.status);
    ASSERT_EQ (st::stateful_error_t::none, objects.commit_create (old.attempt));
    const auto spot = create (st::object_kind_t::user_spot, "old-spot");
    ASSERT_EQ (st::create_status_t::reserved, spot.status);
    ASSERT_EQ (st::stateful_error_t::none, objects.commit_create (spot.attempt));
    auto transfers = objects.actor_transfers ();
    ASSERT_TRUE (transfers->try_reserve_source ("prior"));
    auto inventory = objects.try_begin_maintenance_inventory ();
    ASSERT_FALSE (inventory.await_ready ());
    EXPECT_FALSE (transfers->try_reserve_source ("late"));
    // Only Join claims are sealed while the preceding source finishes.
    EXPECT_EQ (st::stateful_error_t::none, objects.destroy_actor (old.object));
    EXPECT_EQ (st::stateful_error_t::none, objects.close_spot (spot.object).first);
    const auto added = create (st::object_kind_t::actor, "added");
    const auto added_commit = objects.commit_create (added.attempt);
    const st::object_ref_t reserved{
      st::object_kind_t::user_spot, "reserved", 1, 1, mesh_name, first_rid};
    const auto reserved_create = objects.begin_reserved_object (reserved, actor_type, {});
    const auto reserved_commit = objects.commit_create (reserved_create.attempt);
    transfers->cancel_move ("prior");
    const auto snapshot = inventory.result ().value ();
    ASSERT_EQ (st::create_status_t::reserved, added.status);
    ASSERT_EQ (st::stateful_error_t::none, added_commit);
    ASSERT_EQ (st::stateful_error_t::none, reserved_create.error);
    ASSERT_EQ (st::stateful_error_t::none, reserved_commit);
    ASSERT_TRUE (snapshot);
    ASSERT_EQ (2u, snapshot->size ());
    EXPECT_TRUE (std::any_of (snapshot->begin (), snapshot->end (),
                              [&] (const auto &entry) { return entry.owner == added.object; }));
    EXPECT_TRUE (std::any_of (snapshot->begin (), snapshot->end (),
                              [&] (const auto &entry) { return entry.owner == reserved; }));
    EXPECT_EQ (st::stateful_error_t::moving, create (st::object_kind_t::actor, "after").error);
    auto late_reserved = reserved;
    late_reserved.key = "after-reserved";
    EXPECT_EQ (st::stateful_error_t::moving,
               objects.begin_reserved_object (late_reserved, actor_type, {}).error);
    EXPECT_EQ (st::stateful_error_t::moving, objects.destroy_actor (added.object));
    EXPECT_EQ (st::stateful_error_t::moving, objects.close_spot (reserved).first);
    objects.end_maintenance_inventory ().result ().value ();
    EXPECT_TRUE (transfers->try_begin_local ("released"));
    transfers->cancel_move ("released");
    EXPECT_EQ (st::stateful_error_t::none, objects.destroy_actor (added.object));
    EXPECT_EQ (st::stateful_error_t::none, objects.close_spot (reserved).first);
}

TEST (ActorMaintenanceJoin, LocalNextJoinPreservesClaimedNativeSuffixBeforePrivateAdmission)
{
    next_join_suffix (false, false, false, false, true);
}
TEST (ActorMaintenanceJoin, RemoteNextJoinPreservesClaimedNativeSuffixBeforePrivateAdmission)
{
    next_join_suffix (true, false, false, false, true);
}

TEST (NativeIntake, OrdinarySpotDoesNotMaterializeRelocationRecord)
{
    observations_t evidence;
    observations = &evidence;
    evidence.ordinary_spot = std::make_shared<ordinary_spot_observation_t> ();
    const auto observation = evidence.ordinary_spot;
    struct release_t
    {
        std::shared_ptr<ordinary_spot_observation_t> observation;
        ~release_t () { observation->release.complete (fw::result_t<void>::success ()); }
    };
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t host;
    release_t release{observation};
    configure (host, first_rid, store, relocations);
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    auto route = host.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    auto first = route.request_to_spot (fw::spot_id_t (local_spot), probe_t{1}).async<probe_t> ();
    observation->entered.task ().result ().value ();
    const auto registration =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ()).front ();
    auto node = fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
    const auto counts = fw::runtime::host::public_host_runtime_test_access_t::spot_records (*node);
    EXPECT_EQ (counts[0], 0u) << "frozen record before seal";
    EXPECT_EQ (counts[1], 0u) << "summary before seal";
    EXPECT_EQ (counts[2], 0u) << "raw pending registrations before seal";
    observation->release.complete (fw::result_t<void>::success ());
    const auto reply = first.result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 1);
    ASSERT_TRUE (route.send_to_spot (fw::spot_id_t (local_spot), probe_t{2}).async ().result ());
    const auto barrier =
      route.request_to_spot (fw::spot_id_t (local_spot), probe_t{3}).async<probe_t> ().result ();
    ASSERT_TRUE (barrier);
    EXPECT_EQ (barrier.value ().value, 3);
    EXPECT_EQ (observation->handled, (std::vector<int>{1, 2, 3}));
    const auto completed =
      fw::runtime::host::public_host_runtime_test_access_t::spot_records (*node);
    EXPECT_EQ (completed[2], 0u) << "raw pending registrations on ordinary terminal";
}

TEST (NativeIntake, LocalAcceptedRecordReleasesAfterNativeTerminal)
{
    native_intake_fixture_t fixture;
    auto actor = fixture.host->create_actor (actor_type, actor_id);
    auto wire = fixture.actor_record (1);
    auto parts = fw::runtime::protocol::decode_application_parts (std::move (wire.parts.back ()));
    const auto sent = actor.send_to (actor.ref (), parts).result ();
    ASSERT_TRUE (sent);
    ASSERT_EQ (sent.value (), zlink::submit_result_t::ok);
    std::weak_ptr<fw::runtime::serial_application_record_t> accepted;
    std::size_t received = 0;
    auto dispatch = [&] (const fw::runtime::host::ready_record_t &,
                         const fw::runtime::host::receive_record_t &record,
                         std::vector<zlink::message_t>) {
        ++received;
        accepted = record.application_record;
        ASSERT_TRUE (record.application_record);
        ASSERT_TRUE (record.complete_stateful_dispatch);
        record.complete_stateful_dispatch ();
    };
    ASSERT_TRUE (fixture.host->dispatch_ready (dispatch).result ());
    ASSERT_EQ (received, 1u);
    fixture.host->close ();
    EXPECT_TRUE (accepted.expired ());
}

TEST (NativeIntake, CanonicalAdmissionReportsOnlyActualSuccessWhenEnabled)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    for (const auto mode :
         {fw::message_flow_log_mode_t::off, fw::message_flow_log_mode_t::normal}) {
        SCOPED_TRACE (mode == fw::message_flow_log_mode_t::off ? "off" : "normal");
        std::vector<fw::message_flow_event_t> events;
        fw::dispatch_options_t dispatch;
        dispatch.message_flow (mode);
        fw::detail::dispatch_options_access_t::set_observer_for_tests (
          dispatch, [&] (const auto &event) { events.push_back (event); });
        native_intake_fixture_t fixture (std::move (dispatch));
        auto host = fixture.host;
        const st::object_ref_t object{st::object_kind_t::actor,
                                      actor_id,
                                      1,
                                      1,
                                      mesh_name,
                                      zlink::routing_id_t::from (fixture.rid).to_string ()};
        const auto created = host->objects ().begin_reserved_object (object, actor_type, {});
        ASSERT_EQ (created.error, st::stateful_error_t::none);
        ASSERT_EQ (host->objects ().commit_create (created.attempt), st::stateful_error_t::none);
        st::raw_stateful_dispatch_t raw (host->objects (), host->transport (), fixture.resolver);
        int releases = 0;
        const auto ingest = [&] (auto record, const st::object_ref_t &target) {
            const auto result =
              raw
                .ingest_claimed (target, std::move (record),
                                 std::function<void ()> ([&] { ++releases; }),
                                 fw::test::retained_ingress_snapshot (host->objects (), target))
                .result ();
            EXPECT_TRUE (result);
            return result ? result.value () : st::stateful_error_t::invalid;
        };
        EXPECT_NE (ingest (fixture.actor_record (1, 2), object), st::stateful_error_t::none);
        EXPECT_EQ (releases, 1);
        EXPECT_TRUE (events.empty ());
        EXPECT_EQ (host->objects ().pending (object, st::turn_domain_t::application), 0u);
        EXPECT_EQ (ingest (fixture.actor_record (2), object), st::stateful_error_t::none);
        EXPECT_EQ (events.size (), mode == fw::message_flow_log_mode_t::normal ? 1u : 0u);
        if (!events.empty ()) {
            EXPECT_EQ (events.front ().outcome, fw::message_flow_outcome_t::admitted);
            EXPECT_EQ (events.front ().actor_id, actor_id);
            EXPECT_EQ (events.front ().packet_name, probe_t::packet_name);
            EXPECT_FALSE (events.front ().detail_stage);
            EXPECT_FALSE (events.front ().detail_result);
        }
        auto stale_object = object;
        ++stale_object.object_generation;
        // The valid route/body reaches commit; the actual Kernel owner rejects enqueue.
        EXPECT_EQ (ingest (fixture.actor_record (3), stale_object),
                   st::stateful_error_t::generation_stale);
        EXPECT_EQ (releases, 2);
        EXPECT_EQ (events.size (), mode == fw::message_flow_log_mode_t::normal ? 1u : 0u);
        EXPECT_EQ (host->objects ().pending (object, st::turn_domain_t::application), 1u);
        auto [error, delivery] = raw.try_claim (object);
        EXPECT_EQ (error, st::stateful_error_t::none);
        EXPECT_TRUE (delivery);
        if (delivery) {
            EXPECT_EQ (delivery->turn.sequence, 1u);
            EXPECT_EQ (delivery->frozen.operation.low, 2u);
            EXPECT_EQ (raw.complete (*delivery), st::stateful_error_t::none);
        }
        EXPECT_EQ (releases, 3);
        EXPECT_EQ (ingest (fixture.actor_record (4), object), st::stateful_error_t::none);
        auto [following_error, following] = raw.try_claim (object);
        EXPECT_EQ (following_error, st::stateful_error_t::none);
        EXPECT_TRUE (following);
        if (following) {
            EXPECT_EQ (following->turn.sequence, 2u);
            EXPECT_EQ (following->frozen.operation.low, 4u);
            EXPECT_EQ (raw.complete (*following), st::stateful_error_t::none);
        }
        EXPECT_EQ (events.size (), mode == fw::message_flow_log_mode_t::normal ? 2u : 0u);
        EXPECT_EQ (raw.discard_pending (object), st::stateful_error_t::none);
        EXPECT_EQ (releases, 4);
        EXPECT_EQ (host->objects ().pending (object, st::turn_domain_t::application), 0u);
        host->close ();
    }
}

TEST (NativeIntake, JoinedBatchFenceExcludesInitialMoveAndOtherActor)
{
    observations_t evidence;
    evidence.joined_payloads = {1, 2};
    fw::message_flow_event_t event{};
    event.outcome = fw::message_flow_outcome_t::admitted;
    event.actor_id = actor_id;
    event.packet_name = move_t::packet_name;
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 0);
    EXPECT_FALSE (evidence.joined_send_captured.task ().await_ready ());
    int completions = 0;
    evidence.joined_admission_completion = observations_t::joined_admission_batch_t{
      2, [&] {
          ++completions;
          evidence.joined_send_captured.complete (fw::result_t<void>::success ());
      }};
    event.packet_name = probe_t::packet_name;
    event.actor_id = "other-actor";
    observe_joined_batch_admission (evidence, event);
    event.actor_id = actor_id;
    event.outcome = fw::message_flow_outcome_t::received;
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 0);
    event.outcome = fw::message_flow_outcome_t::admitted;
    event.detail_stage = "invoke_erased.post_serial";
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 0);
    event.detail_stage = "canonical_application.commit_accepted_ingress";
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 0);
    ASSERT_TRUE (evidence.joined_admission_completion);
    EXPECT_EQ (evidence.joined_admission_completion->remaining, 1u);
    EXPECT_FALSE (evidence.joined_send_captured.task ().await_ready ());

    event.outcome = fw::message_flow_outcome_t::received;
    event.detail_stage = "relay_actor_packet.try_append_backlog";
    event.detail_result = "result=appended";
    event.actor_id = "other-actor";
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 0);
    event.actor_id = actor_id;
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 1);
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 2);
    EXPECT_EQ (evidence.joined_admission_completion->remaining, 1u);
    EXPECT_FALSE (evidence.joined_send_captured.task ().await_ready ());

    event.outcome = fw::message_flow_outcome_t::admitted;
    event.detail_stage = "canonical_application.commit_accepted_ingress";
    event.detail_result.reset ();
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 2);
    EXPECT_TRUE (evidence.joined_send_captured.task ().await_ready ());
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 2);
    EXPECT_EQ (completions, 1);
    EXPECT_FALSE (evidence.joined_admission_completion);

    event.outcome = fw::message_flow_outcome_t::received;
    event.detail_stage = "relay_actor_packet.try_append_backlog";
    event.detail_result = "result=appended";
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 3);
    EXPECT_EQ (completions, 1);
}

TEST (NativeIntake, TypedControlMalformedWireAndKernelMetadata)
{
    namespace rt = fw::runtime;
    namespace st = fw::runtime::stateful;
    namespace mesh = fw::runtime::mesh;
    namespace wire = fw::runtime::protocol;
    native_intake_fixture_t fixture;
    auto host = fixture.host;
    const auto &rid = fixture.rid;
    const auto &resolver = fixture.resolver;
    // Restore only Kernel metadata: the facade Actor map intentionally has no entry.
    const st::object_ref_t object{st::object_kind_t::actor,
                                  actor_id,
                                  1,
                                  1,
                                  mesh_name,
                                  zlink::routing_id_t::from (rid).to_string ()};
    const auto created = host->objects ().begin_reserved_object (object, actor_type, {});
    ASSERT_EQ (created.error, st::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), st::stateful_error_t::none);
    auto &mailbox = host->transport ().mailbox ();
    const auto owner =
      mesh::service_mailbox_t::application_owner (rt::host::owner_kind_t::actor, actor_id);
    std::size_t started = 0, rejected = 0, controls = 0, applications = 0;
    auto callback = [&] (const rt::host::ready_record_t &ready,
                         const rt::host::receive_record_t &record,
                         std::vector<zlink::message_t> parts) {
        if (record.kind == rt::host::record_kind_t::spot_control) {
            EXPECT_TRUE (parts.empty ());
            ++controls;
        } else {
            ASSERT_TRUE (ready.actor);
            EXPECT_EQ (fw::detail::actor_ref_access_t::actor_type (*ready.actor), actor_type);
            EXPECT_EQ (parts.size (), 1u);
            ++applications;
            ASSERT_TRUE (record.complete_stateful_dispatch);
            record.complete_stateful_dispatch ();
        }
    };
    const auto drain = [&] {
        EXPECT_TRUE (mailbox.begin_application_drain (owner));
        const auto result =
          host->dispatch_application_owner (
                owner, callback, [&] { ++started; }, [&] { ++rejected; })
            .result ();
        EXPECT_TRUE (result);
        if (result)
            EXPECT_TRUE (result.value ());
        mailbox.end_application_drain (owner);
        EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
        EXPECT_EQ (mailbox.pending_bytes (mesh::service_mailbox_domain_t::application), 0u);
    };
    mesh::service_mailbox_record_t typed;
    typed.owner = owner;
    typed.domain = mesh::service_mailbox_domain_t::application;
    typed.application = std::make_shared<rt::host::local_application_dispatch_t> ();
    typed.application->record.kind = rt::host::record_kind_t::spot_control;
    ASSERT_TRUE (mailbox.try_enqueue (std::move (typed)));
    drain ();
    EXPECT_EQ (controls, 1u);
    EXPECT_EQ (rejected, 0u);
    mesh::service_mailbox_record_t malformed;
    malformed.owner = owner;
    malformed.domain = mesh::service_mailbox_domain_t::application;
    malformed.parts.push_back (zlink::message_t::from ("invalid"));
    ASSERT_TRUE (mailbox.try_enqueue (std::move (malformed)));
    drain ();
    EXPECT_EQ (controls, 1u);
    EXPECT_EQ (rejected, 1u);
    st::raw_stateful_dispatch_t raw (host->objects (), host->transport (), resolver);
    int releases = 0;
    EXPECT_EQ (raw
                 .ingest_claimed (object, {}, std::function<void ()> ([&] { ++releases; }),
                                  fw::test::retained_ingress_snapshot (host->objects (), object))
                 .result ()
                 .value (),
               st::stateful_error_t::invalid);
    EXPECT_EQ (releases, 1);
    auto valid = fixture.actor_record (1);
    ASSERT_TRUE (mailbox.try_enqueue (std::move (valid)));
    drain ();
    EXPECT_EQ (applications, 1u);
    EXPECT_EQ (controls, 1u);
    EXPECT_EQ (started, 3u);
    EXPECT_EQ (rejected, 1u);
    EXPECT_EQ (host->objects ().pending (object, st::turn_domain_t::application), 0u);
    host->close ();
}

TEST (NativeIntake, CanonicalPrivateFifoReturnsNativeClaimBeforeTerminal)
{
    namespace rt = fw::runtime;
    namespace wire = rt::protocol;
    for (const bool restored : {false, true}) {
        SCOPED_TRACE (restored ? "restored" : "live");
        fw::test::manual_internal_scheduler_t worker;
        auto queue = std::make_shared<rt::serial_execution_queue_t> (worker);
        auto record = std::make_shared<rt::serial_application_record_t> ();
        record->payload.emplace ();
        wire::application_payload_t::multipart_t parts;
        parts.push_back (zlink::message_t::from ("1"));
        wire::actor_message_header_t header;
        const auto rid = zlink::routing_id_t::from (first_rid).to_bytes ();
        header.target = {actor_id, 1, rid, 1, 1, 1};
        header.operation = {1, 1};
        record->payload->application_record = rt::stateful::accepted_actor_record (
          header, {"source", 1, rid, 1},
          wire::application_payload_t::from_parts (std::move (parts)), false);
        if (restored) {
            record->payload->frozen_record =
              wire::summarize_frozen_application_record (*record->payload->application_record);
            record->payload->application_record.reset ();
        }
        int native_returns = 0, handlers = 0;
        record->transferred = [&] { ++native_returns; };
        rt::serial_execution_queue_t::async_completion_t finish;
        rt::serial_work_options_t options;
        options.application_record = record;
        const bool accepted = queue->try_post_async (
          "native-private-owner",
          [&, record] (auto terminal) {
              record->admission.enter_handler ();
              ++handlers;
              finish = std::move (terminal);
          },
          options);
        EXPECT_TRUE (accepted);
        EXPECT_TRUE (queue->try_post ("private-successor", [&] { ++handlers; }));
        worker.run_all ();
        EXPECT_EQ (handlers, 1);
        EXPECT_EQ (native_returns, 1);
        // Settle the fixture's native source even on the failing input, then
        // end its actual private terminal and drain the successor normally.
        record->release_native_claim ();
        if (finish)
            finish ([] {});
        worker.run_all ();
        EXPECT_EQ (handlers, 2);
        EXPECT_EQ (native_returns, 1);
        queue->close ();
        worker.run_all ();
        queue->drain ();
        worker.retire ();
    }
}

TEST (NativeIntake, NativeMailboxTransferKeepsActualPrivateMClaim)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    namespace mesh = rt::mesh;
    for (const bool kernel_only_producer : {false, true}) {
        SCOPED_TRACE (kernel_only_producer ? "Kernel-only negative" : "native handoff");
        native_intake_fixture_t fixture;
        auto host = fixture.host;
        const st::object_ref_t object{st::object_kind_t::actor,
                                      actor_id,
                                      1,
                                      1,
                                      mesh_name,
                                      zlink::routing_id_t::from (fixture.rid).to_string ()};
        const auto created = host->objects ().begin_reserved_object (object, actor_type, {});
        ASSERT_EQ (created.error, st::stateful_error_t::none);
        ASSERT_EQ (host->objects ().commit_create (created.attempt), st::stateful_error_t::none);
        auto &mailbox = host->transport ().mailbox ();
        const auto owner =
          mesh::service_mailbox_t::application_owner (rt::host::owner_kind_t::actor, actor_id);
        fw::test::manual_internal_scheduler_t worker;
        auto spot = std::make_shared<rt::serial_execution_queue_t> (
          worker, rt::serial_execution_queue_options_t{},
          rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::spot_wide ());
        auto queue = std::make_shared<rt::serial_execution_queue_t> (worker);
        queue->attach_spot_gate (spot);
        fw::task_completion_source_t<void> mailbox_terminal;
        int delivered = 0, handlers = 0, started = 0, rejected = 0;
        struct settle_fixture_t
        {
            fw::task_completion_source_t<void> &terminal;
            fw::test::manual_internal_scheduler_t &worker;
            std::shared_ptr<rt::serial_execution_queue_t> queue;
            std::shared_ptr<rt::serial_execution_queue_t> spot;
            std::shared_ptr<rt::host::public_host_runtime_t> host;
            ~settle_fixture_t ()
            {
                terminal.complete (fw::result_t<void>::success ());
                worker.run_all ();
                queue->close ();
                worker.run_all ();
                queue->drain ();
                spot->close ();
                worker.run_all ();
                spot->drain ();
                worker.retire ();
                host->close ();
            }
        } cleanup{mailbox_terminal, worker, queue, spot, host};
        const auto dispatch = [&] (const rt::host::ready_record_t &,
                                   const rt::host::receive_record_t &record,
                                   std::vector<zlink::message_t> parts) {
            EXPECT_EQ (parts.size (), 1u);
            ++delivered;
            EXPECT_EQ (record.operation_id.low, static_cast<std::uint64_t> (delivered));
            auto accepted = record.application_record;
            EXPECT_TRUE (accepted);
            if (!accepted) {
                if (record.complete_stateful_dispatch)
                    record.complete_stateful_dispatch ();
                return;
            }
            const bool first = delivered == 1;
            if (first && kernel_only_producer) {
                // The previous producer returned only the Kernel claim and
                // kept the actual native mailbox reservation until M.
                accepted->transferred = [host, object] {
                    (void) host->objects ().complete_claim (object, st::turn_domain_t::application);
                };
            }
            rt::serial_work_options_t options;
            options.application_record = accepted;
            EXPECT_TRUE (queue->try_post_async (
              "native-private-M",
              [&, accepted, first, operation = record.operation_id.low,
               terminal = std::function<void ()> (
                 [accepted] { (void) accepted->complete_native (std::nullopt); })] (auto complete) {
                  ++handlers;
                  EXPECT_EQ (operation, static_cast<std::uint64_t> (handlers));
                  accepted->admission.enter_handler ();
                  if (first) {
                      auto pending = std::make_shared<fw::task_t<void>> (mailbox_terminal.task ());
                      fw::detail::observe_task_terminal (*pending,
                                                         [pending, terminal] (const auto &) {
                                                             if (terminal)
                                                                 terminal ();
                                                         });
                      auto turn = fw::detail::capture_current_serial_turn ();
                      EXPECT_TRUE (turn);
                      if (turn)
                          EXPECT_TRUE (
                            turn->defer ([&] { return mailbox_terminal.task (); }, [] {}));
                      complete ([] {});
                  } else {
                      complete ([terminal] {
                          if (terminal)
                              terminal ();
                      });
                  }
              },
              options));
        };
        const auto drain = [&] {
            if (!mailbox.begin_application_drain (owner))
                return false;
            const auto result = host
                                  ->dispatch_application_owner (
                                    owner, dispatch, [&] { ++started; }, [&] { ++rejected; })
                                  .result ();
            mailbox.end_application_drain (owner);
            EXPECT_TRUE (result);
            return result && result.value ();
        };
        EXPECT_TRUE (mailbox.try_enqueue (fixture.actor_record (1)));
        EXPECT_TRUE (drain ());
        worker.run_all ();
        EXPECT_EQ (handlers, 1);
        EXPECT_FALSE (mailbox_terminal.task ().await_ready ());
        EXPECT_TRUE (mailbox.try_enqueue (fixture.actor_record (2)));
        const bool native_available = drain ();
        worker.run_all ();
        const auto require_native_transfer = [&] {
            EXPECT_TRUE (native_available && delivered == 2)
              << "native_available=" << native_available << " delivered=" << delivered;
        };
        int observed_rejections = 0;
        if (kernel_only_producer) {
            testing::TestPartResultArray failures;
            {
                testing::ScopedFakeTestPartResultReporter capture (
                  testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD,
                  &failures);
                require_native_transfer ();
            }
            observed_rejections = failures.size ();
            EXPECT_EQ (failures.size (), 1);
            if (failures.size () == 1)
                EXPECT_TRUE (failures.GetTestPartResult (0).nonfatally_failed ());
        } else {
            require_native_transfer ();
        }
        std::printf ("native-M kernel_only=%d native_available=%d delivered=%d handlers_while_M=%d "
                     "rejected_checks=%d\n",
                     kernel_only_producer, native_available, delivered, handlers,
                     observed_rejections);
        EXPECT_EQ (handlers, 1);
        mailbox_terminal.complete (fw::result_t<void>::success ());
        worker.run_all ();
        if (!native_available)
            EXPECT_TRUE (drain ());
        worker.run_all ();
        EXPECT_EQ (delivered, 2);
        EXPECT_EQ (handlers, 2);
        EXPECT_EQ (started, 2);
        EXPECT_EQ (rejected, 0);
        EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
        EXPECT_EQ (mailbox.pending_bytes (mesh::service_mailbox_domain_t::application), 0u);
        EXPECT_EQ (host->objects ().pending (object, st::turn_domain_t::application), 0u);
        EXPECT_EQ (host->objects ().complete_claim (object, st::turn_domain_t::application),
                   st::stateful_error_t::conflict);
    }
}

// Trap an accidental accepted-source Store lookup while using the production resolver.
class intake_store_t final : public fw::location_store_t
{
  public:
    intake_store_t ()
    {
        // Capture the provider's key through its existing authority conversion.
        capture_key = &authority_store_key;
        fw::runtime::provider_location_repository_t repository (*this);
        repository.read_authority (fw::runtime::actor_authority_key (actor_id)).result ().value ();
        capture_key = nullptr;
    }
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        if (capture_key)
            *capture_key = key.value;
        auto actual = inner.read (key).result ().value ();
        const bool infrastructure = fw::detail::current_infrastructure_wait ();
        int expected = 1;
        if (infrastructure && std::this_thread::get_id () != releaser
            && key.value == authority_store_key && stage.compare_exchange_strong (expected, 0)) {
            authority_value = std::move (actual);
            observe (0);
            return authority_source.task ();
        }
        expected = 3;
        if (std::this_thread::get_id () == releaser
            && stage.compare_exchange_strong (expected, 0)) {
            lease_value = std::move (actual);
            observe (2);
            return lease_source.task ();
        }
        return fw::task_t<fw::store_read_result_t> (
          fw::result_t<fw::store_read_result_t>::success (std::move (actual)));
    }
    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        auto actual = inner.scan (std::move (request)).result ().value ();
        int expected = 2;
        if (std::this_thread::get_id () == releaser
            && stage.compare_exchange_strong (expected, 0)) {
            nodes_value = std::move (actual);
            observe (1);
            return nodes_source.task ();
        }
        return fw::task_t<fw::store_scan_result_t> (
          fw::result_t<fw::store_scan_result_t>::success (std::move (actual)));
    }
    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        return inner.write (std::move (request));
    }
    void observe (std::size_t index) { reached[index].store (true); }
    void release (int index, bool failure)
    {
        stage.store (failure || index == 2 ? 0 : index + 2);
        if (index == 0)
            authority_source.complete (
              failure ? fw::result_t<fw::store_read_result_t>::failure (
                          fw::framework_error_kind_t::internal_failure, "intake authority failure")
                      : fw::result_t<fw::store_read_result_t>::success (*authority_value));
        else if (index == 1)
            nodes_source.complete (
              failure ? fw::result_t<fw::store_scan_result_t>::failure (
                          fw::framework_error_kind_t::internal_failure, "intake nodes failure")
                      : fw::result_t<fw::store_scan_result_t>::success (*nodes_value));
        else
            lease_source.complete (
              failure ? fw::result_t<fw::store_read_result_t>::failure (
                          fw::framework_error_kind_t::internal_failure, "intake lease failure")
                      : fw::result_t<fw::store_read_result_t>::success (*lease_value));
    }
    void release_all ()
    {
        stage.store (0);
        authority_source.complete (fw::result_t<fw::store_read_result_t>::failure (
          fw::framework_error_kind_t::shutting_down, "intake fixture settled"));
        nodes_source.complete (fw::result_t<fw::store_scan_result_t>::failure (
          fw::framework_error_kind_t::shutting_down, "intake fixture settled"));
        lease_source.complete (fw::result_t<fw::store_read_result_t>::failure (
          fw::framework_error_kind_t::shutting_down, "intake fixture settled"));
    }
    ~intake_store_t () override { release_all (); }
    std::string authority_store_key;
    std::string *capture_key = nullptr;
    std::atomic<int> stage{0};
    std::array<std::atomic_bool, 3> reached{};
    const std::thread::id releaser = std::this_thread::get_id ();
    std::optional<fw::store_read_result_t> authority_value, lease_value;
    std::optional<fw::store_scan_result_t> nodes_value;
    fw::task_completion_source_t<fw::store_read_result_t> authority_source, lease_source;
    fw::task_completion_source_t<fw::store_scan_result_t> nodes_source;
    fw::runtime::in_memory_location_store_t inner;
};

class NativeIntakePendingStore : public testing::TestWithParam<std::tuple<bool, int>>
{
};
TEST_P (NativeIntakePendingStore, ProductionResolverUsesNoStoreDuringAcceptance)
{
    const auto [remote, failure_stage] = GetParam ();
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<intake_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t host, peer;
    release_callbacks_t callbacks{evidence};
    configure (host, first_rid, store, relocations);
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    auto &actors = services.get_required<fw::actor_manager_t> ();
    ASSERT_TRUE (actors.get_or_create (fw::actor_id_t (actor_id), actor_type).async ().result ());
    configure (peer, second_rid, store, relocations);
    peer.start ();
    ASSERT_TRUE (ready (peer));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    auto peer_services = peer.app.advanced ().services ().build_provider ();
    auto &local = services.get_required<fw::actor_client_t> ();
    auto &native = peer_services.get_required<fw::actor_client_t> ();
    auto registration =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ()).front ();
    if (failure_stage >= 0)
        store->release (failure_stage, true);
    store->stage.store (1);
    auto pending =
      (remote ? native : local).request (fw::actor_id_t (actor_id), probe_t{17}).async<probe_t> ();
    std::atomic<int> terminals{0};
    std::promise<void> terminal_seen;
    auto terminal_ready = terminal_seen.get_future ();
    fw::detail::observe_task_completion (pending, [&] (const auto &) {
        if (++terminals == 1) {
            terminal_seen.set_value ();
        }
    });
    const auto result = pending.result ();
    EXPECT_TRUE (result) << (result.error () ? result.error ()->what () : "request failed");
    if (result)
        EXPECT_EQ (result.value ().value, 17);
    for (int index = 0; index != 3; ++index)
        EXPECT_FALSE (store->reached[index].load ()) << "unexpected Store read=" << index;
    store->release_all ();
    terminal_ready.get ();
    EXPECT_EQ (terminals.load (), 1);
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_EQ (std::count (evidence.intake_probe_values.begin (),
                               evidence.intake_probe_values.end (), 17),
                   1);
    }
    constexpr int following_payload = 117;
    const auto following = (remote ? native : local)
                             .request (fw::actor_id_t (actor_id), probe_t{following_payload})
                             .async<probe_t> ()
                             .result ();
    EXPECT_TRUE (following);
    if (following)
        EXPECT_EQ (following.value ().value, following_payload);
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_EQ (std::count (evidence.intake_probe_values.begin (),
                               evidence.intake_probe_values.end (), following_payload),
                   1);
    }

    auto jobs = registration->application_jobs;
    const auto node = fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
    host.app.request_stop ();
    peer.app.request_stop ();
    host.thread.join ();
    peer.thread.join ();
    EXPECT_EQ (jobs->snapshot ().permits_in_use, 0u);
    EXPECT_EQ (jobs->snapshot ().capacity_waiters, 0u);
    EXPECT_EQ (node->transport ().mailbox ().pending_messages (
                 fw::runtime::mesh::service_mailbox_domain_t::application),
               0u);
    EXPECT_EQ (node->transport ().mailbox ().pending_bytes (
                 fw::runtime::mesh::service_mailbox_domain_t::application),
               0u);
}
INSTANTIATE_TEST_SUITE_P (LocalAndNative,
                          NativeIntakePendingStore,
                          testing::Combine (testing::Bool (), testing::Values (-1, 0, 1, 2)));

class creation_store_t final : public fw::location_store_t
{
  public:
    std::atomic<bool> fail_next_write{false};
    std::atomic<int> failed_writes{0};
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        const auto terminal_write = std::any_of (
          request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
              const auto *put = std::get_if<fw::store_put_t> (&mutation);
              return put && put->key.value.starts_with (std::string ("creation-terminal") + '\0');
          });
        if (terminal_write && fail_next_write.exchange (false)) {
            ++failed_writes;
            throw fw::framework_exception_t (fw::framework_error_kind_t::internal_failure,
                                             "creation terminal publication failed");
        }
        return inner.write (std::move (request));
    }

  private:
    fw::runtime::in_memory_location_store_t inner;
};
class ActorCreationHandoff : public testing::TestWithParam<creation_case_t>
{
};
TEST_P (ActorCreationHandoff, TerminalReleasesMoveWithoutJoinLifecycle)
{
    observations_t evidence;
    evidence.creation_case = GetParam ();
    observations = &evidence;
    auto store = std::make_shared<creation_store_t> ();
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, first_rid, store,
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    if (GetParam () == creation_case_t::publication_failed)
        store->fail_next_write = true;
    auto services = host.app.advanced ().services ().build_provider ();
    const auto created = services.get_required<fw::actor_manager_t> ()
                           .get_or_create (fw::actor_id_t (actor_id), actor_type)
                           .async ()
                           .result ();
    if (GetParam () == creation_case_t::accepted) {
        ASSERT_TRUE (created);
        const auto first = services.get_required<fw::actor_client_t> ()
                             .request (fw::actor_id_t (actor_id), probe_t{17})
                             .async<probe_t> ()
                             .result ();
        ASSERT_TRUE (first);
        EXPECT_EQ (first.value ().value, 17);
    } else {
        EXPECT_FALSE (created);
    }
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    ASSERT_TRUE (state->worker_executor);
    state->worker_executor->drain ();
    EXPECT_FALSE (
      state->actor_transfer_coordinator->phase (std::string (actor_type) + ":" + actor_id));
    EXPECT_EQ (store->failed_writes.load (),
               GetParam () == creation_case_t::publication_failed ? 1 : 0);
    std::lock_guard lock (evidence.mutex);
    EXPECT_EQ (evidence.creation_callbacks, 1);
    EXPECT_EQ (evidence.entry_joined, 0);
    EXPECT_TRUE (evidence.completions.empty ());
}
INSTANTIATE_TEST_SUITE_P (AcceptedRejectedFailedPublication,
                          ActorCreationHandoff,
                          testing::Values (creation_case_t::accepted,
                                           creation_case_t::rejected,
                                           creation_case_t::failed,
                                           creation_case_t::publication_failed));
TEST (ActorMaintenanceJoin, PublicEntryJoinCompletionPrecedesFirstCommand)
{
    observations_t evidence;
    observations = &evidence;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, first_rid, std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (client, evidence, local_spot, 1));
    ASSERT_TRUE (move_to (client, evidence, entry_target, 2));
    const auto first =
      client.request (fw::actor_id_t (actor_id), probe_t{17}).async<probe_t> ().result ();
    ASSERT_TRUE (first);
    EXPECT_EQ (first.value ().value, 17);
    std::lock_guard lock (evidence.mutex);
    EXPECT_EQ (evidence.creation_callbacks, 1);
    EXPECT_EQ (evidence.entry_joined, 1);
    ASSERT_EQ (evidence.completions.size (), 2u);
    const auto *accepted = std::get_if<fw::actor_join_accepted_t> (&evidence.completions.back ());
    ASSERT_NE (accepted, nullptr);
    EXPECT_TRUE (accepted->operation_id_high != 0 || accepted->operation_id_low != 0);
}

// A Store write is visible before its caller receives the result. Exercise that
// public provider boundary without changing the relocation runtime or its lanes.
class observed_location_store_t final : public fw::location_store_t
{
  public:
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        count_accepted_read (key);
        return _inner.read (std::move (key));
    }
    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        count_accepted_scan ();
        std::function<fw::task_t<void> ()> before_scan;
        {
            std::lock_guard lock (_mutex);
            if (_scan_ready && _scan_ready ())
                before_scan = std::exchange (_before_scan, {});
        }
        if (before_scan)
            co_await before_scan ();
        co_return co_await _inner.scan (std::move (request));
    }
    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        std::function<fw::task_t<bool> (const fw::store_write_request_t &)> ready;
        {
            std::lock_guard lock (_mutex);
            if (_before_write)
                ready = _before_write_ready;
        }
        std::function<fw::task_t<void> ()> before_write;
        if (ready && co_await ready (request)) {
            std::lock_guard lock (_mutex);
            before_write = std::exchange (_before_write, {});
        }
        if (before_write)
            co_await before_write ();
        auto result = co_await _inner.write (std::move (request));
        if (before_write) {
            std::lock_guard lock (_mutex);
            _intercepted_write_conflicted =
              std::holds_alternative<fw::store_write_conflict_t> (result);
        }
        if (std::holds_alternative<fw::store_write_applied_t> (result)) {
            std::function<fw::task_t<void> ()> after_write;
            {
                std::lock_guard lock (_mutex);
                after_write = _after_write;
            }
            if (after_write)
                co_await after_write ();
        }
        co_return result;
    }
    void observe (std::function<fw::task_t<void> ()> after_write)
    {
        std::lock_guard lock (_mutex);
        _after_write = std::move (after_write);
    }
    void before_next_write_when (
      std::function<fw::task_t<bool> (const fw::store_write_request_t &)> ready,
      std::function<fw::task_t<void> ()> action)
    {
        std::lock_guard lock (_mutex);
        _before_write_ready = std::move (ready);
        _before_write = std::move (action);
    }
    void before_scan_when (std::function<bool ()> ready, std::function<fw::task_t<void> ()> action)
    {
        std::lock_guard lock (_mutex);
        _scan_ready = std::move (ready);
        _before_scan = std::move (action);
    }
    bool intercepted_write_conflicted ()
    {
        std::lock_guard lock (_mutex);
        return _intercepted_write_conflicted;
    }
    void observe_authority_reads (const std::string &authority_key)
    {
        std::lock_guard lock (_mutex);
        _observed_authority_key = authority_key;
        _acceptance_thread.reset ();
        _acceptance_reads = _acceptance_scans = 0;
    }
    std::pair<std::size_t, std::size_t> accepted_store_calls ()
    {
        std::lock_guard lock (_mutex);
        return {_acceptance_reads, _acceptance_scans};
    }

  private:
    void count_accepted_read (const fw::store_key_t &key)
    {
        std::lock_guard lock (_mutex);
        if (!_observed_authority_key.empty () && key.value == _observed_authority_key)
            _acceptance_thread = std::this_thread::get_id ();
        if (_acceptance_thread == std::this_thread::get_id ())
            ++_acceptance_reads;
    }
    void count_accepted_scan ()
    {
        std::lock_guard lock (_mutex);
        if (_acceptance_thread == std::this_thread::get_id ())
            ++_acceptance_scans;
    }
    fw::runtime::in_memory_location_store_t _inner;
    std::mutex _mutex;
    std::function<fw::task_t<void> ()> _after_write;
    std::function<fw::task_t<bool> (const fw::store_write_request_t &)> _before_write_ready;
    std::function<fw::task_t<void> ()> _before_write;
    std::function<bool ()> _scan_ready;
    std::function<fw::task_t<void> ()> _before_scan;
    bool _intercepted_write_conflicted = false;
    std::string _observed_authority_key;
    std::optional<std::thread::id> _acceptance_thread;
    std::size_t _acceptance_reads = 0;
    std::size_t _acceptance_scans = 0;
};

class accepted_source_repository_t final : public fw::runtime::in_memory_location_repository_t
{
  public:
    std::optional<fw::owner_lease_read_result_t> lease_override;
    fw::task_t<fw::owner_lease_read_result_t> read_owner_lease (std::string owner_id) override
    {
        if (lease_override)
            return fw::task_t<fw::owner_lease_read_result_t> (
              fw::result_t<fw::owner_lease_read_result_t>::success (*lease_override));
        return in_memory_location_repository_t::read_owner_lease (std::move (owner_id));
    }
};

enum class accepted_source_case_t
{
    live,
    expired,
    wrong_owner,
    stale_node_generation,
    lease_generation_mismatch,
    replaced_live_owner
};

TEST (ActorMaintenanceJoin, RelocationValidatesAcceptedSourcesBeforeRestoringSpotQueue)
{
    using source_case = accepted_source_case_t;
    for (const auto mode :
         {source_case::live, source_case::expired, source_case::wrong_owner,
          source_case::stale_node_generation, source_case::lease_generation_mismatch,
          source_case::replaced_live_owner}) {
        SCOPED_TRACE (static_cast<int> (mode));
        auto store = std::make_shared<accepted_source_repository_t> ();
        const auto token =
          std::get<fw::owner_lease_claimed_t> (
            store->claim_owner_lease ("accepted-source-owner", 15s).result ().value ())
            .token;
        fw::mesh_node_descriptor_t descriptor{.mesh_name = "source-boundary",
                                              .rid = zlink::routing_id_t::from ("source-node"),
                                              .lifecycle_generation = 1,
                                              .descriptor_revision = 1,
                                              .endpoint = "tcp://127.0.0.1:1",
                                              .state = fw::framework_runtime_state_t::serving,
                                              .security_identity = "test",
                                              .owner_id = token.owner_id,
                                              .lease_generation = token.lease_generation};
        ASSERT_EQ (store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   fw::location_write_status_t::stored);
        const auto now = std::chrono::system_clock::now ();
        if (mode == source_case::expired)
            store->lease_override = fw::owner_lease_found_t{token, now, now};
        else if (mode == source_case::wrong_owner) {
            auto other = token;
            other.owner_id = "other-owner";
            store->lease_override = fw::owner_lease_found_t{other, now + 15s, now};
        } else if (mode == source_case::lease_generation_mismatch) {
            auto newer = token;
            ++newer.lease_generation;
            store->lease_override = fw::owner_lease_found_t{newer, now + 15s, now};
        } else if (mode == source_case::replaced_live_owner) {
            const auto replacement =
              std::get<fw::owner_lease_claimed_t> (
                store->claim_owner_lease ("replacement-owner", 15s).result ().value ())
                .token;
            ASSERT_EQ (store->remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, token)
                         .result ()
                         .value (),
                       fw::location_write_status_t::stored);
            descriptor.owner_id = replacement.owner_id;
            descriptor.lease_generation = replacement.lease_generation;
            ASSERT_EQ (store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                         .result ()
                         .value ()
                         .status,
                       fw::location_write_status_t::stored);
        }
        fw::runtime::host::host_options_t options;
        options.mesh.descriptor = {"source-boundary",
                                   zlink::routing_id_t::from ("target-node").to_bytes (),
                                   1,
                                   1,
                                   "tcp://127.0.0.1:0",
                                   {},
                                   fw::runtime::mesh::service_node_state_t::preparing};
        options.mesh.runtime_failures =
          std::make_shared<fw::runtime::runtime_failure_collector_t> ();
        auto host =
          std::make_shared<fw::runtime::host::public_host_runtime_t> (std::move (options));
        const fw::runtime::protocol::request_source_fence_t source{
          token.owner_id, static_cast<std::uint64_t> (token.lease_generation),
          descriptor.rid.to_bytes (), mode == source_case::stale_node_generation ? 2u : 1u};
        int materialized = 0;
        const auto restored =
          fw::runtime::host::public_host_runtime_test_access_t::restore_accepted_spot (
            *host, store, source, materialized)
            .result ();
        ASSERT_TRUE (restored);
        EXPECT_EQ (restored.value (), mode == source_case::live);
        EXPECT_EQ (materialized, mode == source_case::live ? 1 : 0);
        const auto object = host->objects ().resolve_spot ("accepted-spot");
        EXPECT_EQ (object.has_value (), mode == source_case::live);
        if (object)
            EXPECT_EQ (
              host->objects ().pending (*object, fw::runtime::stateful::turn_domain_t::application),
              1u);
    }
}

TEST (ActorMaintenanceJoin, WarmLocalAcceptedRequestDoesNotReadStore)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<observed_location_store_t> ();
    host_t host;
    configure (host, first_rid, store,
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (
      client.request (fw::actor_id_t (actor_id), probe_t{17}).async<probe_t> ().result ());
    // Use the provider's existing key conversion, as the pending-Store fixture does.
    auto captured = std::make_shared<intake_store_t> ();
    store->observe_authority_reads (captured->authority_store_key);
    const auto reply =
      client.request (fw::actor_id_t (actor_id), probe_t{23}).async<probe_t> ().result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 23);
    const auto [reads, scans] = store->accepted_store_calls ();
    EXPECT_EQ (reads, 0u);
    EXPECT_EQ (scans, 0u);
}

TEST (ActorMaintenanceJoin, StaleAttemptActivationDoesNotLeaveRawIdentity)
{
    namespace runtime = fw::runtime;
    for (const auto [replaced, activation_started] :
         {std::pair{false, false}, std::pair{true, false}, std::pair{false, true},
          std::pair{true, true}}) {
        SCOPED_TRACE (activation_started ? "activation queued" : "before activation");
        SCOPED_TRACE (replaced ? "replaced pending assembly" : "removed attempt");
        const auto rid = zlink::routing_id_t::from (second_rid);
        auto host = std::make_shared<runtime::host::public_host_runtime_t> (
          runtime::host::host_options_t{runtime::mesh::raw_mesh_node_options_t{
            .descriptor = {mesh_name,
                           rid.to_bytes (),
                           1,
                           1,
                           "tcp://127.0.0.1:0",
                           {},
                           runtime::mesh::service_node_state_t::preparing},
            .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
        const runtime::stateful::object_ref_t target{
          runtime::stateful::object_kind_t::actor, actor_id, 1, 1, mesh_name, second_rid};
        const auto activated =
          runtime::host::public_host_runtime_test_access_t::activate_stale_actor_attempt (
            *host, replaced, activation_started, target, actor_type)
            .result ();
        ASSERT_FALSE (activated);
        ASSERT_TRUE (activated.error ());
        EXPECT_EQ (activated.error ()->kind (), fw::framework_error_kind_t::shutting_down);
        const auto candidate = runtime::host::public_host_runtime_t::remote_actor_ref (
          rid, actor_id, target.object_generation);
        EXPECT_FALSE (host->resolve_actor (candidate));
    }
}

TEST (ActorMaintenanceJoin, ReadyAuthorityAcceptsLocalSendBeforeStoreWriteReturns)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<observed_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    auto observed = std::make_shared<std::atomic<bool>> (false);
    auto sent = std::make_shared<fw::task_completion_source_t<void>> ();
    host_t source, target;
    configure (source, first_rid, store, relocations, source_follow_duration);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (target, second_rid, store, relocations, source_follow_duration);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto target_services = target.app.advanced ().services ().build_provider ();
    auto &client = target_services.get_required<fw::actor_client_t> ();
    // Callback copies own services/results; evidence outlives both hosts.
    store->observe ([target_services, observed, sent, &evidence] () mutable -> fw::task_t<void> {
        const auto location = co_await target_services.get_required<fw::location_runtime_query_t> ()
                                .find_actor_location (fw::actor_id_t (actor_id));
        if (!location || location->state != fw::location_object_state_t::ready
            || location->node_rid.to_string () != second_rid)
            co_return;
        if (!observed->exchange (true)) {
            std::fprintf (
              stderr, "Store write boundary: Ready ActorId=%s NodeRid=%s ObjectGeneration=%llu\n",
              location->global_id.c_str (), location->node_rid.to_string ().c_str (),
              static_cast<unsigned long long> (location->object_generation));
            const auto admitted = co_await fw::runtime::await_result (
              target_services.get_required<fw::actor_client_t> ()
                .send (fw::actor_id_t (actor_id), record_t{1})
                .async ());
            {
                std::lock_guard lock (evidence.mutex);
                EXPECT_TRUE (evidence.recorded.empty ());
            }
            sent->complete (admitted);
        }
        co_await sent->task ();
    });
    const auto relocated =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result ();
    store->observe ({});
    ASSERT_TRUE (observed->load ());
    ASSERT_TRUE (sent->task ().await_ready ());
    const auto admitted = sent->task ().result ();
    ASSERT_TRUE (admitted) << (admitted.error () ? admitted.error ()->what () : "");
    ASSERT_TRUE (relocated);
    EXPECT_EQ (relocated.value ().outcome, fw::relocation_outcome_t::relocated);
    const auto reply = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                         .timeout (wait_budget)
                         .async<probe_t> ()
                         .result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 2);
    std::unique_lock lock (evidence.mutex);
    EXPECT_TRUE (evidence.changed.wait_for (
      lock, wait_budget, [&] { return evidence.recorded == std::vector<int>{1}; }));
}

TEST (ActorMaintenanceJoin, SourcePreserveDiscardsRawIdentityPreparedBeforeTargetCas)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<observed_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    configure (source, first_rid, store, relocations, source_follow_duration);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    auto &repository = services.get_required<fw::location_repository_t> ();
    const auto key = fw::runtime::actor_authority_key (actor_id);
    const auto original =
      std::get<fw::authority_snapshot_t> (repository.read_authority (key).result ().value ());
    configure (target, second_rid, store, relocations, source_follow_duration);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto target_services = target.app.advanced ().services ().build_provider ();
    auto &query = target_services.get_required<fw::location_runtime_query_t> ();
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (target.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto native = fw::detail::spot_node_runtime_t (state).native_node ();
    const auto candidate = fw::runtime::host::public_host_runtime_t::remote_actor_ref (
      zlink::routing_id_t::from (second_rid), actor_id, original.object_generation);
    auto preserved = std::make_shared<std::promise<bool>> ();
    auto preservation = preserved->get_future ();
    store->before_next_write_when (
      [native, candidate, target_services, services,
       key] (const fw::store_write_request_t &request) mutable -> fw::task_t<bool> {
          // Read-only observation of the raw index; the failure is injected
          // through the Store provider, without a relocation runtime hook.
          if (!native->resolve_actor (candidate))
              co_return false;
          const auto location =
            co_await target_services.get_required<fw::location_runtime_query_t> ()
              .find_actor_location (fw::actor_id_t (actor_id));
          if (!location || location->node_rid.to_string () != first_rid)
              co_return false;
          const auto current = std::get<fw::authority_snapshot_t> (
            co_await services.get_required<fw::location_repository_t> ().read_authority (key));
          // This in-memory Store issues globally distinct versions. Match
          // the Actor row condition without decoding provider keys/payloads.
          co_return std::ranges::any_of (request.conditions, [&] (const auto &condition) {
              const auto *version = std::get_if<fw::store_version_condition_t> (&condition);
              return version && version->expected.value == current.store_version;
          });
      },
      [services, key, payload = original.payload, preserved] () mutable -> fw::task_t<void> {
          const auto current = std::get<fw::authority_snapshot_t> (
            co_await services.get_required<fw::location_repository_t> ().read_authority (key));
          // The existing source Preserve operation is an unchanged-authority
          // CAS. Restore the uncaptured payload so this newer Store fence
          // definitively excludes the pending target CAS (Location §10).
          preserved->set_value (std::holds_alternative<fw::authority_stored_t> (
            co_await services.get_required<fw::location_repository_t> ()
              .compare_exchange_authority (key, current.store_version,
                                           fw::authority_put_t{payload})));
      });
    const auto relocated =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result ();
    store->before_next_write_when ({}, {});
    ASSERT_EQ (preservation.wait_for (wait_budget), std::future_status::ready);
    ASSERT_TRUE (preservation.get ());
    ASSERT_TRUE (relocated);
    EXPECT_NE (relocated.value ().outcome, fw::relocation_outcome_t::relocated);
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget, [&] {
            return std::ranges::find (evidence.destroyed, 2) != evidence.destroyed.end ();
        }));
    }
    EXPECT_TRUE (store->intercepted_write_conflicted ());
    EXPECT_FALSE (native->resolve_actor (candidate));
    native->seal_relocation_targets ().result ().value ();
    EXPECT_FALSE (native->objects ().find (fw::runtime::stateful::object_kind_t::actor, actor_id));
    const auto location = query.find_actor_location (fw::actor_id_t (actor_id)).result ().value ();
    ASSERT_TRUE (location);
    EXPECT_EQ (location->node_rid.to_string (), first_rid);
}

TEST (ActorMaintenanceJoin, HostShutdownClosesOccupiedUserSpot)
{
    observations_t evidence;
    observations = &evidence;
    host_t host;
    configure (host, first_rid, std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    for (const auto &registration :
         fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())) {
        auto state = registration->spot_state;
        state->lane
          .run ([&] {
              const auto original = state->destroy_actor_registry;
              state->destroy_actor_registry = [original, &evidence] (const auto &actor) {
                  {
                      std::lock_guard lock (evidence.mutex);
                      ++evidence.destroy_callbacks;
                  }
                  return original (actor);
              };
          })
          .get ();
    }
    const auto created = services.get_required<fw::spot_manager_t> ()
                           .get_or_create (fw::spot_id_t (local_spot), room_type)
                           .timeout (wait_budget)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (move_to (services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    const auto explicit_close =
      services.get_required<fw::spot_manager_t> ().close (created.value ().spot).result ();
    ASSERT_TRUE (explicit_close);
    EXPECT_FALSE (explicit_close.value ());
    auto &repository = services.get_required<fw::location_repository_t> ();
    const auto before =
      repository.read_authority (fw::runtime::actor_authority_key (actor_id)).result ().value ();
    ASSERT_TRUE (std::holds_alternative<fw::authority_snapshot_t> (before));
    const auto stopped = host.app.shutdown (wait_budget).result ().value ();
    EXPECT_EQ (stopped.outcome, fw::termination_outcome_t::stopped);
    EXPECT_EQ (stopped.reason, fw::termination_reason_t::none);
    {
        std::lock_guard callback_lock (evidence.mutex);
        EXPECT_EQ (evidence.destroy_callbacks, 0);
    }
    const auto after =
      repository.read_authority (fw::runtime::actor_authority_key (actor_id)).result ().value ();
    ASSERT_TRUE (std::holds_alternative<fw::authority_snapshot_t> (after));
    EXPECT_EQ (std::get<fw::authority_snapshot_t> (before).store_version,
               std::get<fw::authority_snapshot_t> (after).store_version);
    std::lock_guard lock (evidence.mutex);
    ASSERT_EQ (evidence.closings.size (), 1);
    EXPECT_EQ (evidence.closings.front ().first, fw::spot_close_reason_t::host_shutdown);
    EXPECT_EQ (evidence.closings.front ().second, 0);
    EXPECT_EQ (evidence.destroyed.size (), 1);
    EXPECT_EQ (evidence.destroy_callbacks, 0);
}

TEST (ActorMaintenanceJoin, RelocationPublishesSafeToShutdown)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    configure (source, first_rid, store, relocations, source_follow_duration);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (target, second_rid, store, relocations, source_follow_duration);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<fw::framework_runtime_status_t> statuses;
    auto &runtime = services.get_required<fw::framework_runtime_t> ();
    const auto initial_callbacks = [&] {
        std::lock_guard lock (evidence.mutex);
        return std::pair{evidence.entry_joined, evidence.entry_left};
    }();
    auto observation = runtime.observe (status_observation_capacity, [&] (const auto &event) {
        {
            std::lock_guard lock (mutex);
            statuses.push_back (event.status);
        }
        changed.notify_all ();
    });
    const auto relocated =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result ()
        .value ();
    ASSERT_EQ (relocated.outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (relocated.reason);
    EXPECT_FALSE (runtime.status ().safe_to_shutdown);
    std::unique_lock lock (mutex);
    EXPECT_TRUE (changed.wait_for (lock, wait_budget, [&] {
        return std::ranges::any_of (statuses, [] (const auto &status) {
            return status.state == fw::framework_runtime_state_t::relocated
                   && status.safe_to_shutdown;
        });
    }));
    lock.unlock ();
    EXPECT_TRUE (runtime.status ().safe_to_shutdown);
    {
        std::lock_guard status_lock (mutex);
        const auto unsafe = std::ranges::find_if (statuses, [] (const auto &status) {
            return status.state == fw::framework_runtime_state_t::relocated
                   && !status.safe_to_shutdown;
        });
        const auto safe = std::ranges::find_if (statuses, [] (const auto &status) {
            return status.state == fw::framework_runtime_state_t::relocated
                   && status.safe_to_shutdown;
        });
        ASSERT_NE (unsafe, statuses.end ());
        ASSERT_NE (safe, statuses.end ());
        EXPECT_LT (unsafe->sequence, safe->sequence);
    }
    observation->close ();
    auto target_services = target.app.advanced ().services ().build_provider ();
    auto &client = target_services.get_required<fw::actor_client_t> ();
    const auto reply = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                         .timeout (wait_budget)
                         .async<probe_t> ()
                         .result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 2);
    {
        std::lock_guard evidence_lock (evidence.mutex);
        EXPECT_EQ (evidence.entry_joined, initial_callbacks.first);
        EXPECT_EQ (evidence.entry_left, initial_callbacks.second);
    }
}

TEST (ActorMaintenanceJoin, ApplicationSignaledSpotCaptureCompletes)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    configure (source, first_rid, store, relocations, source_follow_duration, 0, true);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (target, second_rid, store, relocations, source_follow_duration, 0, true);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto relocation = source.app.relocate (
      {.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget});
    auto client = source.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    ASSERT_TRUE (client.send_to_spot (fw::spot_id_t (local_spot), ready_t{}).async ().result ());
    const auto result = relocation.result_for (wait_budget);
    ASSERT_TRUE (result.has_value ());
    ASSERT_TRUE (*result);
    EXPECT_EQ (result->value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (result->value ().reason);
}

class ActorHostRelocation : public testing::TestWithParam<bool>
{
};

TEST_P (ActorHostRelocation, AfterRemoteActorJoin)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t entry, source, target;
    configure (entry, first_rid, store, relocations, source_follow_duration, 0, true);
    entry.start ();
    ASSERT_TRUE (ready (entry));
    auto entry_services = entry.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (entry_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &entry_options = entry_services.get_required<fw::route_mesh_runtime_options_t> ();
    const auto entry_weight = entry_options.mesh (mesh_name).placement_weight ();
    entry_options.mesh (mesh_name).placement_weight (0);
    configure (source, second_rid, store, relocations, source_follow_duration, 0, true);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (peer_ready (entry_services.get_required<fw::route_mesh_runtime_t> ()));
    ASSERT_TRUE (
      move_to (entry_services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    {
        std::lock_guard lock (evidence.mutex);
        ASSERT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.back ()));
        ASSERT_EQ (std::get<fw::actor_join_accepted_t> (evidence.completions.back ())
                     .actor.node_rid ()
                     .value (),
                   second_rid);
    }
    if (GetParam ()) {
        entry_options.mesh (mesh_name).placement_weight (entry_weight);
    } else {
        configure (target, "join-third", store, relocations, source_follow_duration, 0, true);
        target.start ();
        ASSERT_TRUE (ready (target));
    }
    auto relocation = source.app.relocate (
      {.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget});
    auto &runtime = services.get_required<fw::framework_runtime_t> ();
    const auto readiness_deadline = std::chrono::steady_clock::now () + wait_budget;
    while (runtime.status ().state != fw::framework_runtime_state_t::relocating
           && std::chrono::steady_clock::now () < readiness_deadline)
        std::this_thread::yield ();
    ASSERT_EQ (runtime.status ().state, fw::framework_runtime_state_t::relocating);
    auto client = source.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    ASSERT_TRUE (client.send_to_spot (fw::spot_id_t (local_spot), ready_t{}).async ().result ());
    const auto result = relocation.result_for (wait_budget);
    ASSERT_TRUE (result.has_value ());
    ASSERT_TRUE (*result);
    ASSERT_EQ (result->value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (result->value ().reason);
    // Relocation flow §4.6: once the aggregate relocation commits, a request to the
    // relocated Actor reaches the target activation and replies.
    const auto reply = entry_services.get_required<fw::actor_client_t> ()
                         .request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                         .timeout (wait_budget)
                         .async<probe_t> ()
                         .result ();
    ASSERT_TRUE (reply) << "kind=" << static_cast<int> (reply.error_kind ())
                        << " error=" << (reply.error () ? reply.error ()->what () : "none");
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_EQ (reply.value ().value, evidence.instances);
    }
    // The relocated User Spot stays addressable by its public Id.
    const auto spot = entry_services.get_required<fw::spot_manager_t> ()
                        .get_or_create (fw::spot_id_t (local_spot), room_type)
                        .timeout (wait_budget)
                        .async ()
                        .result ();
    ASSERT_TRUE (spot) << "kind=" << static_cast<int> (spot.error_kind ());
    EXPECT_EQ (spot.value ().state, fw::spot_create_state_t::existing);
}

// Relocation flow §4.6 and Session Actor binding §8.1: after the aggregate
// relocation commits, the Session keeps its physical connection and its bound
// Actor; the next packet on that connection reaches the target activation.
TEST_P (ActorHostRelocation, ExistingSessionRelaysAfterRelocation)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t entry, source, target, session;
    configure (entry, first_rid, store, relocations, source_follow_duration, 0, true);
    entry.start ();
    ASSERT_TRUE (ready (entry));
    auto entry_services = entry.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (entry_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &entry_options = entry_services.get_required<fw::route_mesh_runtime_options_t> ();
    const auto entry_weight = entry_options.mesh (mesh_name).placement_weight ();
    entry_options.mesh (mesh_name).placement_weight (0);
    configure (source, second_rid, store, relocations, source_follow_duration, 0, true);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    configure_session (session, store, relocations);
    session.start ();
    ASSERT_TRUE (ready (session));
    auto session_services = session.app.advanced ().services ().build_provider ();
    auto &session_routes = session_services.get_required<fw::route_mesh_runtime_t> ();
    ASSERT_TRUE (peer_ready (session_routes, first_rid));
    ASSERT_TRUE (peer_ready (session_routes, second_rid));
    ASSERT_TRUE (
      peer_ready (entry_services.get_required<fw::route_mesh_runtime_t> (), session_rid));
    view_client_t client (session_services.get_required<fw::framework_runtime_t> ()
                            .listener_status (fw::listener_kind_t::stream, stream_name)
                            .endpoint);
    ASSERT_TRUE (client.connect ());
    ASSERT_EQ (client.view (), std::optional<int>{1}) << "bound Entry Spot Actor did not reply";
    ASSERT_TRUE (
      move_to (entry_services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    int joined_instance = 0;
    {
        std::lock_guard lock (evidence.mutex);
        joined_instance = evidence.instances;
    }
    ASSERT_EQ (client.view (), std::optional<int>{joined_instance})
      << "joined Actor did not reply on the existing Session";
    if (GetParam ()) {
        entry_options.mesh (mesh_name).placement_weight (entry_weight);
    } else {
        configure (target, "join-third", store, relocations, source_follow_duration, 0, true);
        target.start ();
        ASSERT_TRUE (ready (target));
    }
    auto relocation = source.app.relocate (
      {.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget});
    auto &runtime = services.get_required<fw::framework_runtime_t> ();
    const auto readiness_deadline = std::chrono::steady_clock::now () + wait_budget;
    while (runtime.status ().state != fw::framework_runtime_state_t::relocating
           && std::chrono::steady_clock::now () < readiness_deadline)
        std::this_thread::yield ();
    ASSERT_EQ (runtime.status ().state, fw::framework_runtime_state_t::relocating);
    auto route = source.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    ASSERT_TRUE (route.send_to_spot (fw::spot_id_t (local_spot), ready_t{}).async ().result ());
    const auto result = relocation.result_for (wait_budget);
    ASSERT_TRUE (result.has_value ());
    ASSERT_TRUE (*result);
    ASSERT_EQ (result->value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (result->value ().reason);
    int relocated_instance = 0;
    {
        std::lock_guard lock (evidence.mutex);
        relocated_instance = evidence.instances;
    }
    ASSERT_GT (relocated_instance, joined_instance);
    EXPECT_EQ (client.view (), std::optional<int>{relocated_instance})
      << "relocated Actor did not reply on the existing Session";
}

INSTANTIATE_TEST_SUITE_P (FreshAndPreviousOwner, ActorHostRelocation, testing::Bool ());

TEST (ActorMaintenanceJoin, ReservedLifecycleCaptureKeepsTurn)
{
    for (const bool early : {true, false}) {
        fw::runtime::offload_executor_t executor (2);
        fw::runtime::serial_execution_queue_t queue (executor, {});
        std::shared_ptr<fw::detail::deferred_barrier_t> barrier;
        std::optional<fw::task_t<void>> phase;
        std::shared_ptr<fw::detail::serial_turn_t> capture_turn;
        std::vector<int> order;
        fw::task_completion_source_t<void> initialized;
        const auto capture = [&] () -> fw::task_t<void> {
            capture_turn = fw::detail::capture_current_serial_turn ();
            EXPECT_TRUE (capture_turn && capture_turn->belongs_to (&queue));
            order.push_back (1);
            co_return;
        };
        queue.post ("reserve-capture", [&] {
            barrier = queue.reserve_barrier_next ("relocation-ready", true).value ();
            if (early)
                phase = barrier->run (capture);
            initialized.complete (fw::result_t<void>::success ());
        });
        ASSERT_TRUE (initialized.task ().result_for (wait_budget));
        if (!early)
            phase = barrier->run (capture);
        const auto captured = phase->result_for (wait_budget);
        if (!captured || !*captured)
            barrier->cancel ();
        ASSERT_TRUE (captured && *captured);
        queue.post ("next-lifecycle", [&] { order.push_back (3); },
                    {fw::runtime::serial_work_lane_t::lifecycle});
        const auto resumed =
          barrier
            ->run ([&] () -> fw::task_t<void> {
                EXPECT_EQ (fw::detail::capture_current_serial_turn (), capture_turn);
                order.push_back (2);
                co_return;
            })
            .result_for (wait_budget);
        if (!resumed || !*resumed)
            barrier->cancel ();
        ASSERT_TRUE (resumed && *resumed);
        ASSERT_TRUE (barrier->activate_async ([&] (auto release) {
            EXPECT_EQ (fw::detail::capture_current_serial_turn (), capture_turn);
            release (fw::result_t<void>::success ());
        }));
        queue.drain ();
        EXPECT_EQ (order, (std::vector<int>{1, 2, 3}));
    }
}

TEST (ActorMaintenanceJoin, CancelledReservedCaptureFinishes)
{
    fw::runtime::offload_executor_t executor (2);
    fw::runtime::serial_execution_queue_t queue (executor, {});
    std::optional<fw::task_t<void>> phase;
    bool ran = false;
    fw::task_completion_source_t<void> initialized;
    queue.post ("cancel-capture", [&] {
        const auto barrier = queue.reserve_barrier_next ("relocation-ready", true).value ();
        phase = barrier->run ([&] () -> fw::task_t<void> {
            ran = true;
            co_return;
        });
        barrier->cancel ();
        initialized.complete (fw::result_t<void>::success ());
    });
    ASSERT_TRUE (initialized.task ().result_for (wait_budget));
    const auto result = phase->result_for (wait_budget);
    ASSERT_TRUE (result.has_value ());
    EXPECT_FALSE (*result);
    EXPECT_FALSE (ran);
    queue.drain ();
}

TEST (ActorMaintenanceJoin, LocalDirectAdmissionUsesCurrentActorId)
{
    namespace host = fw::runtime::host;
    namespace mesh = fw::runtime::mesh;
    namespace stateful = fw::runtime::stateful;
    constexpr std::uint64_t current_generation = 2;
    constexpr std::uint64_t stale_generation = 1;
    const auto rid = zlink::routing_id_t::from (first_rid);
    auto node = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{fw::test::runtime_failure_options (
        mesh::raw_mesh_node_options_t{.descriptor = {mesh_name,
                                                     rid.to_bytes (),
                                                     1,
                                                     1,
                                                     "tcp://127.0.0.1:0",
                                                     {},
                                                     mesh::service_node_state_t::preparing}})});
    node->start ();
    auto actor = node->create_reserved_actor (
      actor_type,
      stateful::object_ref_t{stateful::object_kind_t::actor, actor_id, current_generation,
                             current_generation, mesh_name, rid.to_string ()});
    const auto stale =
      host::public_host_runtime_t::remote_actor_ref (rid, actor_id, stale_generation);
    EXPECT_FALSE (node->resolve_actor (stale));
    const std::vector<zlink::message_t> parts{zlink::message_t::from ("local-admission")};
    const auto sent = node->send_to_actor (stale, parts).result ();
    ASSERT_TRUE (sent);
    ASSERT_EQ (sent.value (), zlink::submit_result_t::ok);
    bool delivered = false;
    ASSERT_TRUE (
      node
        ->dispatch_ready ([&] (const host::ready_record_t &owner, const host::receive_record_t &,
                               std::vector<zlink::message_t> received) {
            ASSERT_TRUE (owner.actor);
            EXPECT_EQ (owner.actor->object_generation (), actor.ref ().object_generation ());
            ASSERT_EQ (received.size (), 1);
            EXPECT_EQ (received.front ().to_string (), "local-admission");
            delivered = true;
        })
        .result ());
    EXPECT_TRUE (delivered);
    const auto absent =
      host::public_host_runtime_t::remote_actor_ref (rid, "absent", stale_generation);
    EXPECT_EQ (node->send_to_actor (absent, parts).result ().value (),
               zlink::submit_result_t::not_found);
    node->close ();
}

TEST (ActorMaintenanceJoin, RejectsWhilePreviousSourceLeaveIsPending)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, first_rid, store, relocations);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (previous_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (maintenance_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    auto *sender = &client;
    ASSERT_TRUE (move_to (*sender, evidence, previous_spot, 1));
    configure (second, second_rid, store, relocations);
    second.start ();
    ASSERT_TRUE (ready (second));
    const auto target_state =
      fw::detail::mesh_node_runtime_t::registrations (second.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    fw::detail::spot_node_runtime_t (target_state)
      .native_node ()
      ->objects ()
      .configure_relocation_materialization (
        [target_state] (const auto &frozen, const auto &object, const auto &spot,
                        std::stop_token cancellation) {
            return fw::detail::spot_node_runtime_t (target_state)
              .materialize_relocation_state (frozen, object, spot, cancellation);
        },
        [target_state, &evidence] (const auto &objects) {
            // Admit local work after identity publication, before lifecycle opens.
            std::unique_lock lock (evidence.mutex);
            evidence.materialization_entered = true;
            evidence.changed.notify_all ();
            evidence.changed.wait (
              lock, [&] { return evidence.release_completion.task ().await_ready (); });
            lock.unlock ();
            return fw::detail::spot_node_runtime_t (target_state)
              .commit_relocation_materialization (objects);
        },
        [target_state] (const auto &objects) {
            fw::detail::spot_node_runtime_t (target_state)
              .abort_relocation_materialization (objects);
        });
    auto &routes = services.get_required<fw::route_mesh_runtime_t> ();
    ASSERT_TRUE (peer_ready (routes));
    // First node cannot place this new room; placement is selected by the public weight API.
    services.get_required<fw::route_mesh_runtime_options_t> ().mesh (mesh_name).placement_weight (
      0);
    auto second_services = second.app.advanced ().services ().build_provider ();
    const auto created = second_services.get_required<fw::spot_manager_t> ()
                           .get_or_create (fw::spot_id_t (current_spot), room_type)
                           .timeout (wait_budget)
                           .async ()
                           .result ();
    ASSERT_TRUE (created) << (created.error () ? created.error ()->what () : "no error");
    ASSERT_TRUE (sender->send (fw::actor_id_t (actor_id), move_t{current_spot}).async ().result ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.materialization_entered; }));
        EXPECT_EQ (evidence.completions.size (), 1);
    }
    sender = &second_services.get_required<fw::actor_client_t> ();
    auto submission = std::async (std::launch::async, [&] {
        return sender->send (fw::actor_id_t (actor_id), move_t{maintenance_spot})
          .async ()
          .result ();
    });
    const auto admitted_while_closed =
      submission.wait_for (wait_budget) == std::future_status::ready;
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_EQ (evidence.completions.size (), 1);
    }
    evidence.release_completion.complete (fw::result_t<void>::success ());
    {
        std::lock_guard lock (evidence.mutex);
        evidence.changed.notify_all ();
    }
    const auto submitted = submission.get ();
    ASSERT_TRUE (admitted_while_closed);
    ASSERT_TRUE (submitted) << (submitted.error () ? submitted.error ()->what () : "no error");
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.completions.size () >= 3; }));
    }
    {
        std::lock_guard lock (evidence.mutex);
        ASSERT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (1)));
        EXPECT_TRUE (
          std::holds_alternative<fw::actor_join_rejected_t> (evidence.completions.at (2)))
          << (std::get_if<fw::actor_join_failed_t> (&evidence.completions.at (2))
                ? static_cast<int> (
                    std::get<fw::actor_join_failed_t> (evidence.completions.at (2)).error_kind)
                : -1);
        EXPECT_EQ (evidence.completion_owners.at (2), second_rid);
    }
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (
          evidence.changed.wait_for (lock, wait_budget, [&] { return evidence.leave_entered; }));
    }
    auto reply = sender->request (fw::actor_id_t (actor_id), probe_t{17})
                   .timeout (wait_budget)
                   .async<probe_t> ()
                   .result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 17);
}
class ActorRestoreCommitFailure : public ::testing::TestWithParam<bool>
{
};

TEST_P (ActorRestoreCommitFailure, DiscardsPublishedIdentity)
{
    observations_t evidence;
    observations = &evidence;
    std::optional<bool> published;
    std::optional<fw::actor_ref_t> candidate;
    bool discarded = false;
    bool identity_removed = false;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, first_rid, store, relocations);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (second, second_rid, store, relocations);
    second.start ();
    ASSERT_TRUE (ready (second));
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (second.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto target = fw::detail::spot_node_runtime_t (state).native_node ();
    const auto weak_target = std::weak_ptr (target);
    const auto throw_failure = GetParam ();
    target->objects ().configure_relocation_materialization (
      [state] (const auto &frozen, const auto &object, const auto &spot,
               std::stop_token cancellation) {
          return fw::detail::spot_node_runtime_t (state).materialize_relocation_state (
            frozen, object, spot, cancellation);
      },
      [&, state, weak_target, throw_failure] (const auto &objects) {
          const auto node = weak_target.lock ();
          if (!node)
              return false;
          const auto ref = fw::runtime::host::public_host_runtime_t::remote_actor_ref (
            zlink::routing_id_t::from (objects.front ().node_id), objects.front ().key,
            objects.front ().object_generation);
          const auto current = node->resolve_actor (ref);
          {
              std::lock_guard lock (evidence.mutex);
              candidate = ref;
              published = current && *current == objects.front ();
          }
          evidence.changed.notify_all ();
          // On the old implementation, let its unpublished target finish so
          // the publication assertion fails without stranding its terminal.
          if (!current)
              return fw::detail::spot_node_runtime_t (state).commit_relocation_materialization (
                objects);
          if (throw_failure)
              throw std::runtime_error ("injected restore commit failure");
          return false;
      },
      [&, state, weak_target] (const auto &objects) {
          const auto node = weak_target.lock ();
          const auto removed = !candidate || !node || !node->resolve_actor (*candidate);
          fw::detail::spot_node_runtime_t (state).abort_relocation_materialization (objects);
          {
              std::lock_guard lock (evidence.mutex);
              discarded = true;
              identity_removed = removed;
          }
          evidence.changed.notify_all ();
      });
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    services.get_required<fw::route_mesh_runtime_options_t> ().mesh (mesh_name).placement_weight (
      0);
    auto second_services = second.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (second_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::actor_client_t> ()
                   .send (fw::actor_id_t (actor_id), move_t{current_spot})
                   .async ()
                   .result ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (
          evidence.changed.wait_for (lock, wait_budget, [&] { return published.has_value (); }));
        ASSERT_TRUE (*published);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget, [&] { return discarded; }));
        EXPECT_TRUE (identity_removed);
    }
    target->seal_relocation_targets ().result ().value ();
    EXPECT_FALSE (target->resolve_actor (*candidate));
    EXPECT_FALSE (target->objects ().find (fw::runtime::stateful::object_kind_t::actor, actor_id));
    const std::vector<zlink::message_t> parts{zlink::message_t::from ("failed-restore")};
    EXPECT_EQ (target->send_to_actor (*candidate, parts).result ().value (),
               zlink::submit_result_t::not_found);
}

INSTANTIATE_TEST_SUITE_P (ReturnOrThrow, ActorRestoreCommitFailure, ::testing::Bool ());

TEST (ActorMaintenanceJoin, PreservesEarlierIdentityWhenRestoreIsRejected)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, first_rid, store, relocations);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (second, second_rid, store, relocations);
    second.start ();
    ASSERT_TRUE (ready (second));
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (second.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto target = fw::detail::spot_node_runtime_t (state).native_node ();
    constexpr std::uint64_t previous_generation = 2;
    const fw::runtime::stateful::object_ref_t previous{
      fw::runtime::stateful::object_kind_t::actor,
      actor_id,
      previous_generation,
      previous_generation,
      mesh_name,
      zlink::routing_id_t::from (second_rid).to_string ()};
    auto previous_actor = target->create_reserved_actor (actor_type, previous);
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    services.get_required<fw::route_mesh_runtime_options_t> ().mesh (mesh_name).placement_weight (
      0);
    auto second_services = second.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (second_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (move_to (services.get_required<fw::actor_client_t> (), evidence, current_spot, 1));
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_TRUE (
          std::holds_alternative<fw::actor_join_failed_t> (evidence.completions.front ()));
    }
    const auto current = target->resolve_actor (previous_actor.ref ());
    ASSERT_TRUE (current);
    EXPECT_EQ (*current, previous);
}

TEST (ActorMaintenanceJoin, RemoteJoinReturnsToPreviousSourceNode)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, first_rid, store, relocations);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (local_spot), room_type).async ().result ());
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (source_spot), room_type).async ().result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (client, evidence, local_spot, 1));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (0)));
    EXPECT_EQ (evidence.completion_spots.at (0), fw::spot_id_t (local_spot));
    configure (second, second_rid, store, relocations);
    second.start ();
    ASSERT_TRUE (ready (second));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    services.get_required<fw::route_mesh_runtime_options_t> ().mesh (mesh_name).placement_weight (
      0);
    auto second_services = second.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (second_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (move_to (client, evidence, current_spot, 2));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (1)));
    EXPECT_EQ (evidence.completion_owners.at (1), second_rid);
    EXPECT_EQ (evidence.completion_spots.at (1), fw::spot_id_t (current_spot));
    // Return through the public Deferred Join immediately after Accepted.
    // Source OnLeave and stale instance destruction remain independent.
    ASSERT_TRUE (
      move_to (second_services.get_required<fw::actor_client_t> (), evidence, source_spot, 3));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (2)))
      << (std::get_if<fw::actor_join_failed_t> (&evidence.completions.at (2))
            ? static_cast<int> (
                std::get<fw::actor_join_failed_t> (evidence.completions.at (2)).error_kind)
            : -1);
    EXPECT_EQ (evidence.completion_owners.at (2), first_rid);
    EXPECT_EQ (evidence.completion_spots.at (2), fw::spot_id_t (source_spot));
}

// Session–Actor binding §6/§8: a returning Actor carries its current binding,
// even when this node last hosted it on a different physical Session.
TEST (ActorMaintenanceJoin, ReboundActorReturnsWithCurrentSession)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second, session;
    release_callbacks_t release{evidence};
    const auto configure_public = [&] (host_t &host, const char *rid) {
        auto &options = host.app.add_zlink_framework ();
        options.add_location_store (store);
        options.add_relocation_store (relocations);
        host.app.logging ().use_file (std::string (rid) + ".flow");
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
        auto mesh = options.add_route_mesh (mesh_name);
        mesh.set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (rid))
          .listen ("tcp://127.0.0.1:0");
        mesh.add_entry_spot<entry_t> ([] (fw::entry_spot_context_t context) {
            return std::make_shared<entry_t> (std::move (context));
        });
        mesh
          .add_spot_factory<room_t> (
            room_type,
            [] (fw::spot_context_t context) {
                return std::make_shared<room_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_actor_factory<actor_t, factory_t> (
            actor_type, std::make_shared<factory_t> (),
            [] (auto &factory) { factory.recreate_on_relocation (); });
    };
    configure_public (first, first_rid);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto first_services = first.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (first_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (first_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (source_spot), room_type)
                   .async ()
                   .result ());
    first_services.get_required<fw::route_mesh_runtime_options_t> ()
      .mesh (mesh_name)
      .placement_weight (0);
    configure_public (second, second_rid);
    second.start ();
    ASSERT_TRUE (ready (second));
    auto second_services = second.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (second_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .async ()
                   .result ());
    configure_session (session, store, relocations);
    session.start ();
    ASSERT_TRUE (ready (session));
    auto session_services = session.app.advanced ().services ().build_provider ();
    auto &routes = session_services.get_required<fw::route_mesh_runtime_t> ();
    ASSERT_TRUE (peer_ready (routes, first_rid));
    ASSERT_TRUE (peer_ready (routes, second_rid));
    ASSERT_TRUE (
      peer_ready (first_services.get_required<fw::route_mesh_runtime_t> (), session_rid));
    ASSERT_TRUE (
      peer_ready (second_services.get_required<fw::route_mesh_runtime_t> (), session_rid));
    const auto endpoint = session_services.get_required<fw::framework_runtime_t> ()
                            .listener_status (fw::listener_kind_t::stream, stream_name)
                            .endpoint;
    view_client_t previous (endpoint);
    ASSERT_TRUE (previous.connect ());
    ASSERT_EQ (previous.view (), std::optional<int>{1});
    ASSERT_TRUE (
      move_to (first_services.get_required<fw::actor_client_t> (), evidence, current_spot, 1));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (0)));
    ASSERT_EQ (previous.view (), std::optional<int>{2});
    // A reply on the new connection proves its bind terminal and Actor handler
    // finished before the return Join. No timer or private scheduling hook is used.
    view_client_t current (endpoint);
    ASSERT_TRUE (current.connect ());
    ASSERT_EQ (current.view (), std::optional<int>{2});
    ASSERT_TRUE (
      move_to (second_services.get_required<fw::actor_client_t> (), evidence, source_spot, 2));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (1)));
    EXPECT_EQ (evidence.completion_owners.at (1), first_rid);
    EXPECT_EQ (current.view (), std::optional<int>{3});
}

TEST (ActorMaintenanceJoin, ReturnAdmissionWaitsOutsideTargetLifecycleForSourceTerminal)
{
    observations_t evidence;
    observations = &evidence;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, first_rid, std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    const auto ingress_ready = services.get_required<fw::actor_client_t> ()
                                 .request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                                 .async<probe_t> ()
                                 .result ();
    ASSERT_TRUE (ingress_ready);
    const auto created = services.get_required<fw::spot_manager_t> ()
                           .get_or_create (fw::spot_id_t (source_spot), room_type)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto key = std::string (actor_type) + ":" + actor_id;
    ASSERT_TRUE (
      state->actor_transfer_coordinator->try_begin_source_remote (key, "previous-source"));
    struct finish_source_t
    {
        std::shared_ptr<fw::detail::spot_node_builder_state_t> state;
        std::string key;
        ~finish_source_t () { state->actor_transfer_coordinator->complete_move (key); }
    };
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    auto admission = admitted->task ();
    finish_source_t finish{state, key};
    // Only the coordinator state is fixed here. The target lifecycle and the
    // source-terminal observer use their production dispatch paths.
    fw::detail::spot_node_runtime_t (state).dispatch_wire_actor_join_admission (
      fw::spot_id_t (source_spot), actor_id,
      [admitted] () -> fw::task_t<void> {
          admitted->complete (fw::result_t<void>::success ());
          co_return;
      },
      [admitted] {
          admitted->complete (fw::result_t<void>::failure (fw::framework_error_kind_t::unavailable,
                                                           "return admission was rejected"));
      });
    auto target = state->lane
                    .run ([&] {
                        return fw::detail::spot_context_access_t::state (
                          state->spot_contexts_by_id.at (source_spot));
                    })
                    .get ();
    ASSERT_TRUE (
      target->run_serial_task ("source-terminal-probe", [] () -> fw::task_t<void> { co_return; })
        .result ());
    EXPECT_FALSE (admission.await_ready ())
      << "return admission must remain outside the target lifecycle until the old source ends";
    EXPECT_TRUE (state->actor_transfer_coordinator->finish_move_replay (key).completed);
    EXPECT_TRUE (admission.result ());

    // A target may close while admission is still waiting outside its turn.
    // The same lifecycle submission must reject after source completion.
    ASSERT_TRUE (
      state->actor_transfer_coordinator->try_begin_source_remote (key, "closing-source"));
    auto after_close = std::make_shared<fw::task_completion_source_t<void>> ();
    auto closing_admission = after_close->task ();
    fw::detail::spot_node_runtime_t (state).dispatch_wire_actor_join_admission (
      fw::spot_id_t (source_spot), actor_id,
      [after_close] () -> fw::task_t<void> {
          after_close->complete (fw::result_t<void>::success ());
          co_return;
      },
      [after_close] {
          after_close->complete (fw::result_t<void>::failure (
            fw::framework_error_kind_t::unavailable, "return target closed"));
      });
    ASSERT_TRUE (
      services.get_required<fw::spot_manager_t> ().close (created.value ().spot).result ());
    EXPECT_FALSE (closing_admission.await_ready ());
    EXPECT_TRUE (state->actor_transfer_coordinator->finish_move_replay (key).completed);
    const auto closed = closing_admission.result ();
    ASSERT_FALSE (closed);
    EXPECT_EQ (closed.error_kind (), fw::framework_error_kind_t::unavailable);
}

TEST (ActorMaintenanceJoin, SameNodeMembershipPreservesReturnedActorUntilSourceCleanup)
{
    observations_t evidence;
    evidence.completion_to_hold = 3;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, first_rid, store, relocations, source_follow_duration, 2);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (source_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (local_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    auto *sender = &client;
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (move_to (*sender, evidence, source_spot, 1));

    configure (second, second_rid, store, relocations, source_follow_duration);
    second.start ();
    ASSERT_TRUE (ready (second));
    auto &routes = services.get_required<fw::route_mesh_runtime_t> ();
    ASSERT_TRUE (peer_ready (routes));
    auto second_services = second.app.advanced ().services ().build_provider ();
    const auto created = second_services.get_required<fw::spot_manager_t> ()
                           .get_or_create (fw::spot_id_t (current_spot), room_type)
                           .timeout (wait_budget)
                           .async ()
                           .result ();
    ASSERT_TRUE (created) << (created.error () ? created.error ()->what () : "no error");
    auto source_instance = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                             .timeout (wait_budget)
                             .async<probe_t> ()
                             .result ();
    ASSERT_TRUE (source_instance);
    ASSERT_TRUE (move_to (*sender, evidence, current_spot, 2));
    // Release the first source before creating a newer source-follow route.
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget, [&] {
            return std::find (evidence.destroyed.begin (), evidence.destroyed.end (),
                              source_instance.value ().value)
                   != evidence.destroyed.end ();
        }));
    }
    sender = &second_services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (*sender, evidence, local_spot, 3));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (2)))
      << (std::get_if<fw::actor_join_failed_t> (&evidence.completions.at (2))
            ? static_cast<int> (
                std::get<fw::actor_join_failed_t> (evidence.completions.at (2)).error_kind)
            : -1);
    sender = &client;
    // Submit while the returning Actor's completion still owns its execution
    // turn. Admission must park the payload until that callback has completed.
    const auto submitted =
      sender->send (fw::actor_id_t (actor_id), move_t{source_spot}).async ().result ();
    evidence.release_completion.complete (fw::result_t<void>::success ());
    ASSERT_TRUE (submitted) << (submitted.error () ? submitted.error ()->what () : "no error");
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.completions.size () >= 4; }));
    }
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (3)));
    auto initial = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                     .timeout (wait_budget)
                     .async<probe_t> ()
                     .result ();
    ASSERT_TRUE (initial);
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    do {
        auto current = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                         .timeout (wait_budget)
                         .async<probe_t> ()
                         .result ();
        ASSERT_TRUE (current);
        ASSERT_EQ (current.value ().value, initial.value ().value)
          << "source cleanup replaced the current membership's Actor instance";
        std::this_thread::yield ();
    } while (std::chrono::steady_clock::now () < deadline);
}
}

class ActorDeferredJoinFailure : public testing::TestWithParam<std::tuple<bool, const char *>>
{
};
TEST_P (ActorDeferredJoinFailure, FailureAndRejectionPreserveSourceActor)
{
    observations_t evidence;
    evidence.completion_to_hold = 1;
    evidence.fail_remote_factory =
      std::string_view (std::get<1> (GetParam ())) == factory_failed_spot;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, first_rid, store, relocations);
    fw::detail::dispatch_options_access_t::set_observer_for_tests (
      first.app.add_zlink_framework ().configure_dispatch (),
      [&evidence] (const fw::message_flow_event_t &event) {
          if (event.packet_name != probe_t::packet_name)
              return;
          if (event.actor_id != actor_id || event.outcome != fw::message_flow_outcome_t::admitted
              || event.detail_stage != "canonical_application.commit_accepted_ingress")
              return;
          {
              std::lock_guard lock (evidence.mutex);
              evidence.probe_admission_observed = true;
          }
          evidence.changed.notify_all ();
      });
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (second, second_rid, store, relocations);
    second.start ();
    ASSERT_TRUE (ready (second));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto second_services = second.app.advanced ().services ().build_provider ();
    const auto [remote, target] = GetParam ();
    auto &target_services = remote ? second_services : services;
    auto &other_services = remote ? services : second_services;
    other_services.get_required<fw::route_mesh_runtime_options_t> ()
      .mesh (mesh_name)
      .placement_weight (0);
    auto &spots = target_services.get_required<fw::spot_manager_t> ();
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (target), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    std::size_t initial;
    {
        std::lock_guard lock (evidence.mutex);
        initial = evidence.completions.size ();
    }
    ASSERT_TRUE (move_to (client, evidence, target, initial + 1));
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_EQ (evidence.completion_owners.back (), first_rid);
        if (evidence.fail_remote_factory)
            EXPECT_GT (evidence.remote_factory_failure_count, 0);
        if (std::string_view (target) == rejected_spot) {
            const auto *rejected =
              std::get_if<fw::actor_join_rejected_t> (&evidence.completions.back ());
            ASSERT_NE (rejected, nullptr);
            ASSERT_TRUE (rejected->reply);
            EXPECT_EQ (rejected->reply->decode<std::string> (), rejection_reply);
        } else {
            const auto *failed =
              std::get_if<fw::actor_join_failed_t> (&evidence.completions.back ());
            ASSERT_NE (failed, nullptr)
              << "completion variant=" << evidence.completions.back ().index ();
            EXPECT_EQ (failed->error_kind, std::string_view (target) == framework_failed_spot
                                             ? fw::framework_error_kind_t::unavailable
                                             : fw::framework_error_kind_t::internal_failure);
        }
    }
    // Keep completion's barrier active until the public probe reaches source
    // admission. Existing Detailed flow events fix the order without a sleep.
    auto pending_probe = client.request (fw::actor_id_t (actor_id), probe_t{17})
                           .timeout (wait_budget)
                           .async<probe_t> ();
    bool reached_admission;
    {
        std::unique_lock lock (evidence.mutex);
        reached_admission = evidence.changed.wait_for (
          lock, wait_budget, [&] { return evidence.probe_admission_observed; });
    }
    evidence.release_completion.complete (fw::result_t<void>::success ());
    ASSERT_TRUE (reached_admission);
    auto probe = pending_probe.result ();
    ASSERT_TRUE (probe) << "kind=" << static_cast<int> (probe.error_kind ())
                        << " message=" << (probe.error () ? probe.error ()->what () : "");
    EXPECT_EQ (probe.value ().value, 17);
}

INSTANTIATE_TEST_SUITE_P (LocalAndRemote,
                          ActorDeferredJoinFailure,
                          testing::Values (std::make_tuple (false, failed_spot),
                                           std::make_tuple (false, framework_failed_spot),
                                           std::make_tuple (false, rejected_spot),
                                           std::make_tuple (true, failed_spot),
                                           std::make_tuple (true, framework_failed_spot),
                                           std::make_tuple (true, rejected_spot),
                                           std::make_tuple (true, factory_failed_spot)));

TEST (ActorMaintenanceJoin, CompletionCallbackPreservesStandardException)
{
    auto node =
      fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (first_rid);
    fw::detail::spot_node_runtime_t runtime (node);
    const auto actor = fw::detail::actor_ref_access_t::make (
      fw::node_rid_t::from_string (first_rid), actor_type, actor_id, 1);
    node->actor_instances.emplace (std::string (actor_type) + ":" + actor_id,
                                   std::make_shared<int> (0));
    fw::detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    auto release = std::make_shared<fw::task_completion_source_t<void>> ();
    factory.on_join_completed = [release] (void *, auto, auto, auto, const auto *, const auto &,
                                           auto, bool) -> fw::task_t<void> {
        co_await release->task ();
        throw std::runtime_error ("completion callback failed");
    };
    node->actor_factories.emplace (actor_type, std::move (factory));
    auto delivery = runtime.deliver_actor_join_completion (
      actor, fw::actor_join_failed_t{1, 2, fw::framework_error_kind_t::unavailable}, std::nullopt);
    release->complete (fw::result_t<void>::success ());
    const auto result = delivery.result ().value ();
    ASSERT_FALSE (result);
    ASSERT_TRUE (result.exception ());
    try {
        std::rethrow_exception (result.exception ());
        FAIL () << "callback exception was lost";
    }
    catch (const fw::framework_exception_t &) {
        FAIL () << "callback exception was replaced";
    }
    catch (const std::runtime_error &error) {
        EXPECT_STREQ (error.what (), "completion callback failed");
    }
}

namespace
{
// Actor model §6.3 (#1772): a Create or GetOrCreate that makes a new Actor keeps only the nodes
// whose Entry Spot accepts that Actor type (descriptor capability `entryAccepted`).
constexpr auto entryless_weight = 1000;
constexpr auto entry_weight = 1;
constexpr std::size_t descriptor_scan_limit = 1000;

std::string entryless_target_id (std::string seed)
{
    for (;;) {
        const auto choice = std::hash<std::string>{}(seed) % (entryless_weight + entry_weight);
        if (choice > 0 && choice < static_cast<std::size_t> (entryless_weight))
            return seed;
        seed.push_back ('_');
    }
}

std::vector<std::string> factory_nodes (observations_t &evidence)
{
    std::lock_guard lock (evidence.mutex);
    return evidence.factory_nodes;
}

// The `entryAccepted` value of each actor capability this mesh's descriptors publish, keyed by
// routing ID and Actor stable type. A capability without the field reads as `nullopt`.
std::map<std::pair<std::string, std::string>, std::optional<bool>>
published_entry_accepted (fw::location_store_t &store)
{
    std::map<std::pair<std::string, std::string>, std::optional<bool>> result;
    const auto scanned =
      store.scan (fw::store_scan_request_t{{}, std::nullopt, descriptor_scan_limit}).result ();
    const auto &page = std::get<fw::store_scan_page_t> (scanned.value ());
    for (const auto &item : page.items) {
        const auto bytes = std::string (reinterpret_cast<const char *> (item.value.bytes.data ()),
                                        item.value.bytes.size ());
        const auto record = nlohmann::json::parse (bytes, nullptr, false);
        if (!record.is_object () || !record.contains ("descriptor")
            || !record.at ("descriptor").contains ("objectCapabilities"))
            continue;
        const auto &descriptor = record.at ("descriptor");
        if (descriptor.value ("meshName", std::string{}) != mesh_name)
            continue;
        for (const auto &capability : descriptor.at ("objectCapabilities")) {
            if (capability.at ("objectKind") != "actor")
                continue;
            result[{descriptor.at ("routingIdHex").get<std::string> (),
                    capability.at ("stableType").get<std::string> ()}] =
              capability.contains ("entryAccepted")
                ? std::make_optional (capability.at ("entryAccepted").get<bool> ())
                : std::nullopt;
        }
    }
    return result;
}

std::string rid_hex (std::string_view rid)
{
    return zlink::routing_id_t::from (std::string (rid)).to_hex ();
}

// Location Store view of a requester that read its descriptors before a node dropped its Entry
// Spot: every actor capability it lists claims `entryAccepted`.
class stale_entry_descriptor_store_t final : public fw::location_store_t
{
  public:
    explicit stale_entry_descriptor_store_t (
      std::shared_ptr<fw::location_store_t> inner,
      std::optional<std::string> missing_owner = std::nullopt) :
        _inner (std::move (inner)), _missing_owner (std::move (missing_owner))
    {
    }

    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        auto result = co_await _inner->read (std::move (key));
        if (_missing_owner) {
            if (const auto *found = std::get_if<fw::store_found_t> (&result)) {
                const auto record = nlohmann::json::parse (
                  std::string (reinterpret_cast<const char *> (found->value.bytes.data ()),
                               found->value.bytes.size ()),
                  nullptr, false);
                // The canonical owner lease row has these three fields; its expiry is Store metadata.
                if (record.is_object () && record.size () == owner_lease_field_count
                    && record.contains ("recordVersion") && record.contains ("leaseGeneration")
                    && record.value ("ownerId", std::string{}) == *_missing_owner)
                    co_return fw::store_missing_t{found->value.store_now};
            }
        }
        co_return result;
    }

    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        return _inner->write (std::move (request));
    }

    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        auto scanned = co_await _inner->scan (std::move (request));
        if (auto *page = std::get_if<fw::store_scan_page_t> (&scanned)) {
            for (auto &item : page->items) {
                auto record = nlohmann::json::parse (
                  std::string (reinterpret_cast<const char *> (item.value.bytes.data ()),
                               item.value.bytes.size ()),
                  nullptr, false);
                if (!record.is_object () || !record.contains ("descriptor")
                    || !record.at ("descriptor").contains ("objectCapabilities"))
                    continue;
                for (auto &capability : record["descriptor"]["objectCapabilities"])
                    if (capability.at ("objectKind") == "actor")
                        capability["entryAccepted"] = true;
                const auto text = record.dump ();
                item.value.bytes.assign (reinterpret_cast<const std::byte *> (text.data ()),
                                         reinterpret_cast<const std::byte *> (text.data ())
                                           + text.size ());
            }
        }
        co_return scanned;
    }

  private:
    static constexpr std::size_t owner_lease_field_count = 3;
    std::shared_ptr<fw::location_store_t> _inner;
    const std::optional<std::string> _missing_owner;
};

class ActorEntryPlacement : public testing::TestWithParam<entry_kind_t>
{
};

// A node without an Entry Spot for the Actor type (none, or one for another type) is not a
// creation candidate even when its placement weight dominates; the Actor is created on the node
// whose Entry Spot accepts it, and the Entry-less node never runs the factory.
TEST_P (ActorEntryPlacement, CreatesOnlyWhereEntryAcceptsActorType)
{
    observations_t evidence;
    observations = &evidence;
    release_callbacks_t release{evidence};
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t accepting, entryless;
    configure (accepting, first_rid, store, relocations, default_follow_duration, 0, false,
               entry_kind_t::member, entry_weight);
    accepting.start ();
    ASSERT_TRUE (ready (accepting));
    configure (entryless, second_rid, store, relocations, default_follow_duration, 0, false,
               GetParam (), entryless_weight);
    entryless.start ();
    ASSERT_TRUE (ready (entryless));
    auto services = entryless.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto &actors = services.get_required<fw::actor_manager_t> ();
    for (const auto *seed : {"placement-a", "placement-b", "placement-c"}) {
        // Either descriptor order selects the Entry-less node before the fix.
        const auto id = entryless_target_id (seed);
        auto call = std::string_view (seed) == "placement-b"
                      ? actors.create (fw::actor_id_t (id), actor_type)
                      : actors.get_or_create (fw::actor_id_t (id), actor_type);
        const auto created = call.async ().result ();
        ASSERT_TRUE (created) << id << ": "
                              << (created.error () ? created.error ()->what () : "no error");
        const auto *made = std::get_if<fw::actor_create_created_t> (&created.value ());
        ASSERT_NE (made, nullptr) << id;
        EXPECT_EQ (made->actor.node_rid ().value (), first_rid) << id;
    }
    EXPECT_EQ (factory_nodes (evidence), (std::vector<std::string> (3, first_rid)));
}

INSTANTIATE_TEST_SUITE_P (NoEntryAndOtherTypeEntry,
                          ActorEntryPlacement,
                          testing::Values (entry_kind_t::none, entry_kind_t::other));

TEST (ActorEntryPlacementUnavailable, NoAcceptingEntryReturnsUnavailableBeforeFactory)
{
    observations_t evidence;
    observations = &evidence;
    host_t entryless;
    configure (entryless, first_rid, std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> (),
               default_follow_duration, 0, false, entry_kind_t::none);
    entryless.start ();
    ASSERT_TRUE (ready (entryless));
    auto services = entryless.app.advanced ().services ().build_provider ();
    const auto created = services.get_required<fw::actor_manager_t> ()
                           .create (fw::actor_id_t (actor_id), actor_type)
                           .async ()
                           .result ();
    ASSERT_FALSE (created);
    EXPECT_EQ (created.error_kind (), fw::framework_error_kind_t::unavailable);
    EXPECT_TRUE (factory_nodes (evidence).empty ());
}

// Location runtime canonical descriptor: `entryAccepted` is true only for an actor capability
// whose Actor type the node's Entry Spot accepts.
TEST (ActorEntryPlacementDescriptor, PublishesEntryAccepted)
{
    observations_t evidence;
    observations = &evidence;
    release_callbacks_t release{evidence};
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t accepting, entryless, other;
    configure (accepting, first_rid, store, relocations);
    accepting.start ();
    ASSERT_TRUE (ready (accepting));
    configure (entryless, second_rid, store, relocations, default_follow_duration, 0, false,
               entry_kind_t::none);
    entryless.start ();
    ASSERT_TRUE (ready (entryless));
    configure (other, third_rid, store, relocations, default_follow_duration, 0, false,
               entry_kind_t::other);
    other.start ();
    ASSERT_TRUE (ready (other));
    const auto published = published_entry_accepted (*store);
    const std::map<std::pair<std::string, std::string>, std::optional<bool>> expected{
      {{rid_hex (first_rid), actor_type}, true},
      {{rid_hex (second_rid), actor_type}, false},
      {{rid_hex (third_rid), actor_type}, false},
      {{rid_hex (third_rid), other_actor_type}, true}};
    EXPECT_EQ (published, expected);
}

enum class placement_requester_t
{
    accepting,
    entryless,
    third_node
};

class ActorEntryPlacementStaleDescriptor : public testing::TestWithParam<placement_requester_t>
{
};

// A requester whose descriptor read still claims `entryAccepted` for an Entry-less node selects
// it. That target ends the creation before the factory, and the requester excludes that host
// execution generation and creates the Actor on the accepting node within the same call.
// The requester is the Entry-less target, the accepting target, or a third node. These exercise
// local-to-remote, remote-to-local, and remote-to-remote reselection with one operation identity.
TEST_P (ActorEntryPlacementStaleDescriptor, TargetRejectsBeforeFactoryAndRequesterReselects)
{
    observations_t evidence;
    observations = &evidence;
    release_callbacks_t release{evidence};
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    std::shared_ptr<fw::location_store_t> actual_store = store;
    std::shared_ptr<fw::location_store_t> stale =
      std::make_shared<stale_entry_descriptor_store_t> (store);
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    const bool local_target = GetParam () == placement_requester_t::entryless;
    host_t accepting, entryless, third;
    configure (accepting, first_rid, local_target ? actual_store : stale, relocations,
               default_follow_duration, 0, false, entry_kind_t::member, entry_weight);
    accepting.start ();
    ASSERT_TRUE (ready (accepting));
    configure (entryless, second_rid, local_target ? stale : actual_store, relocations,
               default_follow_duration, 0, false, entry_kind_t::none, entryless_weight);
    entryless.start ();
    ASSERT_TRUE (ready (entryless));
    if (GetParam () == placement_requester_t::third_node) {
        configure (third, third_rid, stale, relocations, default_follow_duration, 0, false,
                   entry_kind_t::none, 0);
        third.start ();
        ASSERT_TRUE (ready (third));
    }
    auto &requester = GetParam () == placement_requester_t::third_node ? third
                      : local_target                                   ? entryless
                                                                       : accepting;
    auto services = requester.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), {},
                             GetParam () == placement_requester_t::third_node ? 2 : 1));
    const auto id = entryless_target_id (actor_id);
    const auto created = services.get_required<fw::actor_manager_t> ()
                           .get_or_create (fw::actor_id_t (id), actor_type)
                           .async ()
                           .result ();
    ASSERT_TRUE (created) << (created.error () ? created.error ()->what () : "no error");
    const auto *made = std::get_if<fw::actor_create_created_t> (&created.value ());
    ASSERT_NE (made, nullptr);
    EXPECT_EQ (made->actor.node_rid ().value (), first_rid);
    EXPECT_EQ (factory_nodes (evidence), (std::vector<std::string>{first_rid}));
}

INSTANTIATE_TEST_SUITE_P (LocalAndRemoteTarget,
                          ActorEntryPlacementStaleDescriptor,
                          testing::Values (placement_requester_t::accepting,
                                           placement_requester_t::entryless,
                                           placement_requester_t::third_node));

TEST (ActorEntryPlacementExisting, RequiresLiveOwnerWithoutNewCreationCandidates)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t accepting, live_view, missing_owner_view;
    configure (accepting, first_rid, store, relocations);
    accepting.start ();
    ASSERT_TRUE (ready (accepting));
    auto accepting_services = accepting.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (accepting_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    accepting_services.get_required<fw::route_mesh_runtime_options_t> ()
      .mesh (mesh_name)
      .placement_weight (0);
    const auto current = accepting_services.get_required<fw::location_repository_t> ()
                           .read_authority (fw::runtime::actor_authority_key (actor_id))
                           .result ();
    ASSERT_TRUE (current);
    const auto *snapshot = std::get_if<fw::authority_snapshot_t> (&current.value ());
    ASSERT_NE (snapshot, nullptr);
    auto missing =
      std::make_shared<stale_entry_descriptor_store_t> (store, snapshot->owner.owner_id);
    configure (live_view, second_rid, store, relocations, default_follow_duration, 0, false,
               entry_kind_t::none, 0);
    live_view.start ();
    ASSERT_TRUE (ready (live_view));
    configure (missing_owner_view, third_rid, missing, relocations, default_follow_duration, 0,
               false, entry_kind_t::none, 0);
    missing_owner_view.start ();
    ASSERT_TRUE (ready (missing_owner_view));
    auto live_services = live_view.app.advanced ().services ().build_provider ();
    const auto existing = live_services.get_required<fw::actor_manager_t> ()
                            .get_or_create (fw::actor_id_t (actor_id), actor_type)
                            .async ()
                            .result ();
    ASSERT_TRUE (existing);
    EXPECT_TRUE (std::holds_alternative<fw::actor_create_existing_t> (existing.value ()));
    auto missing_services = missing_owner_view.app.advanced ().services ().build_provider ();
    const auto unavailable = missing_services.get_required<fw::actor_manager_t> ()
                               .get_or_create (fw::actor_id_t (actor_id), actor_type)
                               .async ()
                               .result ();
    ASSERT_FALSE (unavailable);
    EXPECT_EQ (unavailable.error_kind (), fw::framework_error_kind_t::unavailable);
    EXPECT_EQ (factory_nodes (evidence), (std::vector<std::string>{first_rid}));
}

// Regression: an Entry-less node still takes an existing Actor as a join target and, with its
// User Spot, as a relocation target.
TEST (ActorEntryPlacementRegression, EntrylessNodeTakesJoinAndRelocation)
{
    observations_t evidence;
    observations = &evidence;
    release_callbacks_t release{evidence};
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t accepting, source, target;
    configure (accepting, first_rid, store, relocations, source_follow_duration, 0, true);
    accepting.start ();
    ASSERT_TRUE (ready (accepting));
    auto accepting_services = accepting.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (accepting_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    accepting_services.get_required<fw::route_mesh_runtime_options_t> ()
      .mesh (mesh_name)
      .placement_weight (0);
    configure (source, second_rid, store, relocations, source_follow_duration, 0, true,
               entry_kind_t::none);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    // Existing does not need an eligible new placement candidate.
    const auto existing = services.get_required<fw::actor_manager_t> ()
                            .get_or_create (fw::actor_id_t (actor_id), actor_type)
                            .async ()
                            .result ();
    ASSERT_TRUE (existing);
    ASSERT_TRUE (std::holds_alternative<fw::actor_create_existing_t> (existing.value ()));
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (peer_ready (accepting_services.get_required<fw::route_mesh_runtime_t> ()));
    ASSERT_TRUE (
      move_to (accepting_services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    {
        std::lock_guard lock (evidence.mutex);
        ASSERT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.back ()));
        ASSERT_EQ (std::get<fw::actor_join_accepted_t> (evidence.completions.back ())
                     .actor.node_rid ()
                     .value (),
                   second_rid);
    }
    configure (target, third_rid, store, relocations, source_follow_duration, 0, true,
               entry_kind_t::none);
    target.start ();
    ASSERT_TRUE (ready (target));
    auto relocation = source.app.relocate (
      {.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget});
    auto &runtime = services.get_required<fw::framework_runtime_t> ();
    const auto readiness_deadline = std::chrono::steady_clock::now () + wait_budget;
    while (runtime.status ().state != fw::framework_runtime_state_t::relocating
           && std::chrono::steady_clock::now () < readiness_deadline)
        std::this_thread::yield ();
    ASSERT_EQ (runtime.status ().state, fw::framework_runtime_state_t::relocating);
    auto client = source.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    ASSERT_TRUE (client.send_to_spot (fw::spot_id_t (local_spot), ready_t{}).async ().result ());
    const auto result = relocation.result_for (wait_budget);
    ASSERT_TRUE (result.has_value ());
    ASSERT_TRUE (*result);
    EXPECT_EQ (result->value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (result->value ().reason);
    auto probe = accepting_services.get_required<fw::actor_client_t> ()
                   .request (fw::actor_id_t (actor_id), probe_t{7})
                   .timeout (wait_budget)
                   .async<probe_t> ()
                   .result ();
    ASSERT_TRUE (probe) << (probe.error () ? probe.error ()->what () : "no error");
    EXPECT_EQ (probe.value ().value, 7);
}

TEST (ActorMaintenanceJoin, HostRelocationAcquiresUnitsAfterEarlierDeferredJoin)
{
#if !defined(__linux__)
    GTEST_SKIP () << "Requires the public Linux thread inventory to identify the relocation caller";
#else
    constexpr auto remaining_actor = "remaining-member";
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<observed_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, joined, target;
    release_callbacks_t release{evidence};
    configure (source, first_rid, store, relocations);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    auto &actors = services.get_required<fw::actor_manager_t> ();
    auto &client = services.get_required<fw::actor_client_t> ();
    std::map<std::string, std::uint64_t> generations;
    for (const auto *id : {actor_id, remaining_actor}) {
        const auto created =
          actors.get_or_create (fw::actor_id_t (id), actor_type).async ().result ();
        ASSERT_TRUE (created);
        const auto *new_actor = std::get_if<fw::actor_create_created_t> (&created.value ());
        ASSERT_NE (new_actor, nullptr);
        generations.emplace (id, new_actor->actor.object_generation ());
    }
    services.get_required<fw::route_mesh_runtime_options_t> ().mesh (mesh_name).placement_weight (
      0);
    configure (joined, second_rid, store, relocations, default_follow_duration, 0, true);
    joined.start ();
    ASSERT_TRUE (ready (joined));
    auto joined_services = joined.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (joined_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    joined_services.get_required<fw::route_mesh_runtime_options_t> ()
      .mesh (mesh_name)
      .placement_weight (0);
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    configure (target, third_rid, store, relocations);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), third_rid, 2));
    evidence.hold_join_admission = true;
    ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), move_t{local_spot}).async ().result ());
    ASSERT_TRUE (evidence.join_admission_entered.task ().result_for (wait_budget));
    auto &runtime = services.get_required<fw::framework_runtime_t> ();
    // The topology scanner threads are already running. This fixture submits
    // no new application work before Relocate creates its operation worker.
    // Identify that caller at the public Store boundary, so a background scan
    // cannot release the Join before the Host acquires its units.
    std::set<long> existing_threads;
    for (const auto &thread : std::filesystem::directory_iterator ("/proc/self/task"))
        existing_threads.insert (std::stol (thread.path ().filename ().string ()));
    store->before_scan_when (
      [&] {
          return !existing_threads.contains (::syscall (SYS_gettid))
                 && runtime.status ().state == fw::framework_runtime_state_t::relocating;
      },
      [&] () -> fw::task_t<void> {
          evidence.release_join_admission.complete (fw::result_t<void>::success ());
          co_await evidence.join_completed.task ();
      });
    const auto relocated =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result ();
    store->before_scan_when ({}, {});
    ASSERT_TRUE (relocated);
    ASSERT_EQ (relocated.value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (relocated.value ().reason);
    for (const auto &[id, owner] :
         {std::pair{actor_id, second_rid}, std::pair{remaining_actor, third_rid}}) {
        const auto resolved = joined_services.get_required<fw::actor_manager_t> ()
                                .get_or_create (fw::actor_id_t (id), actor_type)
                                .async ()
                                .result ();
        ASSERT_TRUE (resolved);
        const auto *existing = std::get_if<fw::actor_create_existing_t> (&resolved.value ());
        ASSERT_NE (existing, nullptr);
        EXPECT_EQ (existing->actor.node_rid ().value (), owner);
        EXPECT_EQ (existing->actor.object_generation (), generations.at (id));
        const auto reply = joined_services.get_required<fw::actor_client_t> ()
                             .request (fw::actor_id_t (id), probe_t{7})
                             .timeout (wait_budget)
                             .async<probe_t> ()
                             .result ();
        ASSERT_TRUE (reply);
        EXPECT_EQ (reply.value ().value, 7);
    }
#endif
}

TEST (ActorMaintenanceJoin, RemoteJoinDoesNotLeaveLocalRelocationWork)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    release_callbacks_t release{evidence};
    configure (source, first_rid, store, relocations, default_follow_duration, 0, true);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    services.get_required<fw::route_mesh_runtime_options_t> ().mesh (mesh_name).placement_weight (
      0);
    configure (target, second_rid, store, relocations, default_follow_duration, 0, true);
    target.start ();
    ASSERT_TRUE (ready (target));
    auto target_services = target.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (target_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    ASSERT_TRUE (move_to (services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    {
        std::lock_guard lock (evidence.mutex);
        ASSERT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.back ()));
        ASSERT_EQ (std::get<fw::actor_join_accepted_t> (evidence.completions.back ())
                     .actor.node_rid ()
                     .value (),
                   second_rid);
    }
    auto registration =
      fw::detail::mesh_node_runtime_t::registrations (source.app.advanced ().zlink ()).front ();
    const auto source_released_authority = [&] {
        return registration->spot_state->lane
          .run ([&] {
              for (const auto &[key, route] : registration->spot_state->actor_routes) {
                  if (route.node_rid.value () == second_rid
                      && !registration->spot_state->actor_authority_fences.contains (key))
                      return true;
              }
              return false;
          })
          .get ();
    };
    const auto source_completion_deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!source_released_authority ()
           && std::chrono::steady_clock::now () < source_completion_deadline)
        std::this_thread::yield ();
    ASSERT_TRUE (source_released_authority ());
    const auto refs =
      fw::detail::spot_node_runtime_t (registration->spot_state).local_actor_refs ();
    const auto authority_read = services.get_required<fw::location_repository_t> ()
                                  .read_authority (fw::runtime::actor_authority_key (actor_id))
                                  .result ()
                                  .value ();
    const auto *authority = std::get_if<fw::authority_snapshot_t> (&authority_read);
    ASSERT_NE (authority, nullptr);
    EXPECT_EQ (authority->allocation.target.node_rid.value (), second_rid);
    EXPECT_TRUE (refs.empty ());
    EXPECT_TRUE (source_released_authority ());
    const auto result =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result_for (wait_budget);
    ASSERT_TRUE (result && *result);
    EXPECT_EQ (result->value ().outcome, fw::relocation_outcome_t::relocated);
    EXPECT_EQ (result->value ().reason, fw::relocation_reason_t::none);
}
}

namespace
{
TEST (ActorMaintenanceJoin, ActiveInventoryExcludesCreatingRecoveringAndFinalizedSource)
{
    namespace stateful = fw::runtime::stateful;
    const auto make_node = [] (std::string rid) {
        const std::vector<std::uint8_t> routing_bytes (rid.begin (), rid.end ());
        fw::runtime::mesh::service_node_descriptor_t descriptor{
          mesh_name,
          routing_bytes,
          1,
          1,
          "tcp://127.0.0.1:0",
          {},
          fw::runtime::mesh::service_node_state_t::preparing};
        auto native = std::make_shared<fw::runtime::host::public_host_runtime_t> (
          fw::runtime::host::host_options_t{fw::test::runtime_failure_options (
            fw::runtime::mesh::raw_mesh_node_options_t{.descriptor = std::move (descriptor)})});
        auto state =
          fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (mesh_name);
        state->snapshot.name = mesh_name;
        state->snapshot.routing_id = zlink::routing_id_t::from (rid);
        state->native_node = native;
        return std::pair{std::move (native), std::move (state)};
    };
    auto [source_native, source_state] = make_node (first_rid);
    auto &source = source_native->objects ();
    const fw::detail::spot_node_runtime_t source_runtime (source_state);
    const stateful::object_ref_t owner{
      stateful::object_kind_t::actor, actor_id, 1, 1, mesh_name, first_rid};
    const auto materialized_key = std::string (actor_type) + ":" + actor_id;
    const auto reserved = source.begin_reserved_object (owner, actor_type, {});
    ASSERT_EQ (reserved.error, stateful::stateful_error_t::none);
    source_state->lane
      .run (
        [&] { source_state->actor_instances.emplace (materialized_key, std::make_shared<int> ()); })
      .get ();
    EXPECT_TRUE (source_runtime.local_actor_refs ().empty ());
    EXPECT_EQ (source_runtime.monitoring_counts_async ().result ().value ().first, 0u);
    source_state->lane.run ([&] { source_state->actor_instances.erase (materialized_key); }).get ();
    ASSERT_EQ (source.commit_create (reserved.attempt), stateful::stateful_error_t::none);
    EXPECT_TRUE (source_runtime.local_actor_refs ().empty ());
    source_state->lane
      .run (
        [&] { source_state->actor_instances.emplace (materialized_key, std::make_shared<int> ()); })
      .get ();
    const auto active = source_runtime.local_actor_refs ();
    ASSERT_EQ (active.size (), 1u);
    EXPECT_EQ (fw::detail::actor_ref_access_t::actor_type (active.front ()), actor_type);
    EXPECT_EQ (active.front ().object_generation (), owner.object_generation);
    EXPECT_EQ (active.front ().actor_id ().value (), owner.key);
    EXPECT_EQ (source.inventory ().front ().owner, owner);
    EXPECT_EQ (active.front ().node_rid ().value (), owner.node_id);
    EXPECT_EQ (source_runtime.monitoring_counts_async ().result ().value ().first, 1u);

    {
        auto [remote_native, remote_state] = make_node (first_rid);
        auto &remote_objects = remote_native->objects ();
        const fw::detail::spot_node_runtime_t remote_runtime (remote_state);
        const stateful::object_ref_t remote{
          stateful::object_kind_t::actor, "remote-inventory", 1, 1, mesh_name, second_rid};
        const auto remote_reserved = remote_objects.begin_reserved_object (remote, actor_type, {});
        ASSERT_EQ (remote_reserved.error, stateful::stateful_error_t::none);
        ASSERT_EQ (remote_objects.commit_create (remote_reserved.attempt),
                   stateful::stateful_error_t::none);
        const auto remote_key = std::string (actor_type) + ":" + remote.key;
        remote_state->lane
          .run (
            [&] { remote_state->actor_instances.emplace (remote_key, std::make_shared<int> ()); })
          .get ();
        EXPECT_EQ (remote_objects.inventory ().front ().owner, remote);
        EXPECT_TRUE (remote_runtime.local_actor_refs ().empty ());
        EXPECT_EQ (remote_runtime.monitoring_counts_async ().result ().value ().first, 0u);
    }

    const stateful::object_ref_t destination{
      stateful::object_kind_t::user_spot, local_spot, 1, 1, mesh_name, second_rid};
    const auto move = source.begin_remote_membership_move (owner, destination);
    ASSERT_EQ (move.first, stateful::stateful_error_t::none);
    EXPECT_EQ (source_runtime.local_actor_refs ().size (), 1u);
    ASSERT_EQ (source.abort_membership_move (move.second), stateful::stateful_error_t::none);

    const auto sealed = source.try_seal_relocation_aggregate ({owner}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    EXPECT_EQ (source_runtime.local_actor_refs ().size (), 1u);
    ASSERT_EQ (source.begin_relocation_boundary (sealed.seal.token).first,
               stateful::stateful_error_t::none);
    ASSERT_EQ (source.finalize_relocation_cutover (sealed.seal.token),
               stateful::stateful_error_t::none);
    ASSERT_EQ (source.inventory ().size (), 1u);
    EXPECT_TRUE (
      source.finalized_relocation_source (owner.kind, owner.key, owner.object_generation));
    EXPECT_TRUE (source_runtime.local_actor_refs ().empty ());
    EXPECT_EQ (source_runtime.monitoring_counts_async ().result ().value ().first, 0u);

    auto [target_native, target_state] = make_node (second_rid);
    auto &target = target_native->objects ();
    const fw::detail::spot_node_runtime_t target_runtime (target_state);
    EXPECT_TRUE (target_runtime.local_actor_refs ().empty ());
    auto relocated = owner;
    relocated.node_id = second_rid;
    ++relocated.authority_owner_generation;
    const stateful::relocation_restore_identity_t identity{"active-inventory-relocation"};
    ASSERT_EQ (target.restore_relocation (sealed.seal.participants.front (), relocated, identity)
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    const auto staged = target.inventory ();
    ASSERT_EQ (staged.size (), 1u);
    EXPECT_EQ (staged.front ().owner, relocated);
    EXPECT_EQ (staged.front ().state, stateful::object_state_t::recovering);
    target_state->lane
      .run (
        [&] { target_state->actor_instances.emplace (materialized_key, std::make_shared<int> ()); })
      .get ();
    EXPECT_TRUE (target_runtime.local_actor_refs ().empty ());
    EXPECT_EQ (target_runtime.monitoring_counts_async ().result ().value ().first, 0u);
    ASSERT_EQ (target.commit_relocation_restore (relocated, identity),
               stateful::stateful_error_t::none);
    const auto committed = target_runtime.local_actor_refs ();
    ASSERT_EQ (committed.size (), 1u);
    EXPECT_EQ (fw::detail::actor_ref_access_t::actor_type (committed.front ()), actor_type);
    EXPECT_EQ (committed.front ().object_generation (), relocated.object_generation);
    EXPECT_EQ (committed.front ().actor_id ().value (), relocated.key);
    EXPECT_EQ (target.inventory ().front ().owner, relocated);
    EXPECT_EQ (committed.front ().node_rid ().value (), relocated.node_id);
    EXPECT_EQ (target_runtime.monitoring_counts_async ().result ().value ().first, 1u);
    EXPECT_TRUE (source_runtime.local_actor_refs ().empty ());
}
} // namespace


namespace deferred_join_reservation_probe
{
namespace fw = zlink::framework;
namespace rt = fw::runtime;
namespace detail = fw::detail;
constexpr auto packet_name = "deferred-join-reservation-probe";
constexpr auto actor_key = "probe:actor";
constexpr auto first_transfer = "first-remote-join";
constexpr std::size_t replay_worker_count = 2;

std::shared_ptr<rt::serial_application_record_t>
accepted_record (const std::shared_ptr<rt::application_job_queue_t> &jobs, std::uint64_t sequence)
{
    rt::messaging::envelope_header_t envelope;
    envelope.kind = rt::messaging::message_kind_t::command;
    envelope.channel_name = "actor";
    envelope.message_name = packet_name;
    envelope.content_type = "application/json";
    auto parts = rt::messaging::envelope_codec_t{}.encode_raw_body_parts (
      envelope, zlink::message_t::from ("{}"));
    const auto routing_id = zlink::routing_id_t::from ("probe-source").to_bytes ();
    rt::protocol::actor_message_header_t header;
    header.operation = {1, sequence};
    header.target = {"actor", 1, routing_id, 1, 1, 1};
    auto record = std::make_shared<rt::serial_application_record_t> ();
    record->payload.emplace ();
    record->payload->sequence = sequence;
    record->payload->application_payload_bytes = 2;
    // This isolated fixture injects the accepted authority and journal input.
    record->payload->application_record = rt::stateful::accepted_actor_record (
      header, {"probe-owner", 1, routing_id, 1},
      rt::protocol::application_payload_t::from_parts (std::move (parts).take_items ()), false);
    record->admission.jobs = jobs;
    return record;
}

enum class replay_case_t
{
    remote_without_barrier,
    remote_with_barrier,
    local_target,
    rejected_deferred
};

void restore_probe (replay_case_t test_case, bool multiple_packets = false)
{
    const bool with_source_barrier = test_case != replay_case_t::remote_without_barrier;
    const bool local_target = test_case == replay_case_t::local_target;
    const bool rejected_deferred = test_case == replay_case_t::rejected_deferred;
    auto mesh = fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("probe-mesh");
    auto node = mesh->spot_state;
    node->monitoring = fw::test::runtime_failure_monitoring ();
    auto scheduler = std::make_shared<fw::test::manual_internal_scheduler_t> ();
    scheduler->publication_fence (
      fw::test::state_lane_publication_fence (node->lane, node->lane_executor));
    node->worker_executor = scheduler;
    fw::serializer_registry_t serializers;
    fw::service_collection_t services;
    node->root_services = services.build_provider ();
    mesh->application_jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{
        fw::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    auto native = std::make_shared<rt::host::public_host_runtime_t> (rt::host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "probe-mesh",
                              .node_routing_id =
                                zlink::routing_id_t::from ("probe-source").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .application_jobs = mesh->application_jobs,
               .runtime_failures = node->runtime_failures ()},
      .object_stable_types = {"framework.spot", "probe"}});
    native->start ();
    node->native_node = native;
    auto held = mesh->application_jobs->try_reserve_supply ();
    ASSERT_TRUE (held);
    held->mark_queued ();

    auto spot = std::make_shared<detail::spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = fw::node_rid_t::from_string ("probe-source");
    spot->spot_id = fw::spot_id_t ("probe-source-spot");
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime = fw::test::runtime_failure_fixture<detail::channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, detail::spot_context_access_t::create (spot));
    detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.configure_instance = [] (void *, const fw::actor_ref_t &, void *) {};
    node->actor_factories.emplace ("probe", std::move (factory));
    node->actor_instances.emplace (actor_key, std::make_shared<int> (1));
    node->actor_spot_ids.emplace (actor_key, spot->spot_id);
    node->actor_generations.emplace (actor_key, 1);
    node->actor_types_by_id.emplace ("actor", "probe");
    const auto actor = detail::actor_ref_access_t::make (
      fw::node_rid_t::from_string ("probe-source"), "probe", "actor", 1);

    struct observation_t
    {
        bool next_reserved;
        std::string error;
        std::uint32_t permits;
        std::string transfer_id;
    };
    std::vector<std::string> order;
    bool sequence_seen = false;
    std::promise<void> sequence_finished;
    auto sequence_done = sequence_finished.get_future ();
    std::shared_ptr<detail::deferred_barrier_t> next_barrier;
    bool handler_seen = false;
    std::vector<std::shared_ptr<detail::serial_turn_t>> replay_turns;
    fw::task_completion_source_t<observation_t> observed;
    auto handler_finished = observed.task ();
    spot->handlers.push_back (fw::spot_handler_descriptor_t{
      fw::spot_handler_kind_t::actor_send, packet_name, "", std::type_index (typeid (int)),
      std::type_index (typeid (void)), std::type_index (typeid (int)),
      std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&, node] (void *, void *, fw::service_provider_t &, fw::serializer_registry_t &,
                 const zlink::message_t &,
                 const fw::spot_inbound_message_t &metadata) -> fw::task_t<zlink::message_t> {
          if (multiple_packets && metadata.values.contains ("second")) {
              order.push_back ("second");
              co_return zlink::message_t{};
          }
          if (multiple_packets)
              order.push_back ("first");
          try {
              auto next = detail::spot_node_runtime_t (node).reserve_actor_join_barrier (actor);
              observation_t value{
                static_cast<bool> (next), next ? "" : next.error ()->what (),
                mesh->application_jobs->snapshot ().permits_in_use,
                node->actor_transfer_coordinator->transfer_id (actor_key).value_or ("")};
              if (next) {
                  if (multiple_packets) {
                      next_barrier = next.value ();
                      EXPECT_TRUE (detail::defer_current_serial_turn (
                        [&] {
                            order.push_back ("next-barrier");
                            sequence_finished.set_value ();
                            sequence_seen = true;
                            return fw::task_t<void> (fw::result_t<void>::success ());
                        },
                        [&] { next_barrier->cancel (); }, next_barrier));
                  } else if (local_target) {
                      next_barrier = next.value ();
                  } else {
                      next.value ()->cancel ();
                  }
              }
              handler_seen = true;
              observed.complete (fw::result_t<observation_t>::success (std::move (value)));
          }
          catch (...) {
              handler_seen = true;
              observed.complete (detail::current_exception_result<observation_t> ());
          }
          co_return zlink::message_t{};
      });
    node->dispatch.message_flow (fw::message_flow_log_mode_t::detailed);
    const auto observe_flow = [&] (const fw::message_flow_event_t &event) {
        std::fprintf (
          stderr, "FLOW packet=%s stage=%s result=%s\n", event.packet_name.value_or ("").c_str (),
          event.detail_stage.value_or ("").c_str (), event.detail_result.value_or ("").c_str ());
        if (event.packet_name == packet_name && event.detail_stage == "invoke_erased.post_serial") {
            if (auto turn = detail::capture_current_serial_turn ())
                replay_turns.push_back (std::move (turn));
        }
    };
    if (!rejected_deferred)
        detail::dispatch_options_access_t::set_observer_for_tests (node->dispatch, observe_flow);
    std::shared_ptr<detail::deferred_barrier_t> source_barrier;
    detail::deferred_barrier_t::async_completion_t finish_barrier;
    bool source_entered = false;
    std::promise<void> barrier_entered;
    auto entered = barrier_entered.get_future ();
    struct cleanup_t
    {
        std::function<void ()> action;
        ~cleanup_t ()
        {
            if (action)
                action ();
        }
    } cleanup{[&] {
        if (next_barrier)
            next_barrier->cancel ();
        if (finish_barrier) {
            auto complete = std::exchange (finish_barrier, {});
            complete (fw::result_t<void>::success ());
        }
        held.reset ();
        mesh->application_jobs->stop ();
        scheduler->run_all ();
        node->worker_executor->request_stop ();
        node->worker_executor->drain ();
        native->close ();
        node->native_node.reset ();
        detail::dispatch_options_access_t::set_observer_for_tests (node->dispatch, {});
        (void) node->actor_transfer_coordinator->cancel_move (actor_key);
        node->spot_contexts_by_id.clear ();
        spot->node.reset ();
    }};
    if (with_source_barrier) {
        if (local_target) {
            auto previous = std::make_shared<detail::spot_context_state_t> ();
            previous->node = node;
            previous->spot_id = fw::spot_id_t ("probe-previous-spot");
            previous->serial_executor = node->worker_executor;
            node->spot_contexts_by_id.emplace (previous->spot_id,
                                               detail::spot_context_access_t::create (previous));
            node->actor_spot_ids[actor_key] = previous->spot_id;
        }
        auto reserved = detail::spot_node_runtime_t (node).reserve_actor_join_barrier (actor);
        ASSERT_TRUE (reserved);
        source_barrier = std::move (reserved.value ());
        ASSERT_TRUE (source_barrier->activate_async ([&] (auto complete) {
            finish_barrier = std::move (complete);
            barrier_entered.set_value ();
            source_entered = true;
        }));
        scheduler->run_all ();
        ASSERT_TRUE (source_entered);
        node->lane.run ([&] { node->actor_spot_ids[actor_key] = spot->spot_id; }).get ();
    } else {
        ASSERT_TRUE (
          node->actor_transfer_coordinator->try_reserve_source (actor_key, first_transfer));
    }
    if (!rejected_deferred) {
        ASSERT_TRUE (local_target ? node->actor_transfer_coordinator->try_begin_local (actor_key)
                                  : node->actor_transfer_coordinator->try_begin_source_remote (
                                      actor_key, first_transfer));
    }
    std::optional<fw::task_t<std::optional<zlink::message_t>>> rejected_work;
    if (rejected_deferred) {
        auto initial = std::make_shared<rt::application_job_queue_t::permit_t> (std::move (*held));
        held.reset ();
        rejected_work.emplace (detail::spot_node_runtime_t (node).relay_actor_packet (
          actor, detail::actor_gateway_runtime_t{}.actor_context (actor),
          detail::stream_message_kind_t::send, packet_name, zlink::message_t::from ("{}"),
          *node->root_services, serializers, {}, nullptr,
          [initial] { initial->release_for_handler_entry (); }, {},
          zlink::routing_id_t::from ("probe-source"), {1, 1}, 0, std::nullopt, {}, 0, {},
          accepted_record (mesh->application_jobs, 1)));
        detail::dispatch_options_access_t::set_observer_for_tests (node->dispatch, observe_flow);
    } else {
        held.reset ();
        ASSERT_EQ (
          detail::handoff_append_result_t::appended,
          node->actor_transfer_coordinator->try_append_backlog (
            actor_key, detail::handoff_packet_t{packet_name,
                                                {},
                                                "application/json",
                                                {},
                                                false,
                                                accepted_record (mesh->application_jobs, 1)}));
    }

    if (multiple_packets) {
        ASSERT_EQ (
          detail::handoff_append_result_t::appended,
          node->actor_transfer_coordinator->try_append_backlog (
            actor_key, detail::handoff_packet_t{packet_name,
                                                {},
                                                "application/json",
                                                {{"second", "true"}},
                                                false,
                                                accepted_record (mesh->application_jobs, 2)}));
        auto queue = spot->ensure_spot_serial_executor ()->actor_executor (actor_key)->queue ();
        ASSERT_TRUE (queue->try_post ("ordinary-suffix", [&] { order.push_back ("ordinary"); }));
    }

    if (rejected_deferred) {
        auto complete = std::exchange (finish_barrier, {});
        complete (fw::result_t<void>::success ());
    } else {
        detail::spot_node_runtime_t (node).fail_remote_actor_transfer (actor, false, std::nullopt);
    }
    scheduler->run_all ();
    if (local_target) {
        scheduler->run_until (handler_finished);
        scheduler->run_all ();
        EXPECT_TRUE (handler_seen);
    }
    if (finish_barrier) {
        auto complete = std::exchange (finish_barrier, {});
        complete (fw::result_t<void>::success ());
    }
    scheduler->run_all ();
    scheduler->run_until (handler_finished);
    scheduler->run_all ();
    const auto handler_before_cleanup = handler_seen;
    EXPECT_TRUE (handler_before_cleanup);
    if (!handler_before_cleanup) {
        // A bad nested post can leave an accepted replay holding its own
        // execution claim. Release those fixture-owned turns for cleanup.
        for (std::size_t index = 0; index < replay_turns.size (); ++index) {
            auto turn = replay_turns[index];
            (void) turn->release ();
            scheduler->run_all ();
        }
    }
    ASSERT_TRUE (handler_seen);
    const auto &value = handler_finished.result ().value ();
    if (multiple_packets && value.next_reserved) {
        scheduler->run_all ();
        EXPECT_TRUE (sequence_seen);
        auto terminal = next_barrier->terminal ();
        EXPECT_TRUE (terminal.await_ready ());
        if (terminal.await_ready ())
            EXPECT_TRUE (terminal.result ());
        auto completed = node->actor_transfer_coordinator->wait_for_source_completion (actor_key);
        EXPECT_TRUE (completed.await_ready ());
        if (completed.await_ready ())
            completed.result ().value ();
        EXPECT_EQ (spot->ensure_spot_serial_executor ()
                     ->actor_executor (actor_key)
                     ->queue ()
                     ->pending_count (),
                   0u);
    }
    if (local_target && value.next_reserved) {
        EXPECT_FALSE (value.transfer_id.empty ());
        EXPECT_EQ (value.transfer_id, node->actor_transfer_coordinator->transfer_id (actor_key));
        EXPECT_EQ (detail::actor_move_phase_t::source_reserved,
                   node->actor_transfer_coordinator->phase (actor_key));
        next_barrier->cancel ();
        next_barrier.reset ();
    }
    scheduler->run_all ();
    EXPECT_EQ (spot->ensure_spot_serial_executor ()->spot_queue ()->pending_count (), 0u);
    EXPECT_EQ (
      spot->ensure_spot_serial_executor ()->actor_executor (actor_key)->queue ()->pending_count (),
      0u);
    std::fprintf (stdout,
                  "prefix-terminal handler_before_cleanup=%d sequence=%d replay_turns=%zu\n",
                  handler_before_cleanup, sequence_seen, replay_turns.size ());
    const auto phase_after_restore = node->actor_transfer_coordinator->phase (actor_key);
    auto control = detail::spot_node_runtime_t (node).reserve_actor_join_barrier (actor);
    const bool after_restore_reserved = static_cast<bool> (control);
    if (control)
        control.value ()->cancel ();
    cleanup.action ();
    cleanup.action = {};
    const auto remaining = mesh->application_jobs->snapshot ().permits_in_use;
    std::fprintf (stderr,
                  "OBSERVATION source_barrier=%d local_target=%d rejected_deferred=%d "
                  "next_reserved=%d error=%s "
                  "permits_during_handler=%u "
                  "after_restore_reserved=%d final_permits=%u\n",
                  with_source_barrier, local_target, rejected_deferred, value.next_reserved,
                  value.error.c_str (), value.permits, after_restore_reserved, remaining);
    EXPECT_FALSE (phase_after_restore.has_value ());
    EXPECT_TRUE (after_restore_reserved);
    EXPECT_EQ (0u, value.permits);
    EXPECT_EQ (0u, remaining);
    EXPECT_TRUE (value.next_reserved) << value.error;
    if (multiple_packets)
        EXPECT_EQ ((std::vector<std::string>{"first", "next-barrier", "second", "ordinary"}),
                   order);
}
TEST (DeferredJoinReservation, RemoteRestoreWithoutSourceBarrier)
{
    restore_probe (replay_case_t::remote_without_barrier);
}
TEST (DeferredJoinReservation, RemoteRestoreWithSourceBarrier)
{
    restore_probe (replay_case_t::remote_with_barrier);
}
TEST (DeferredJoinReservation, LocalTargetWithPreviousSpotBarrier)
{
    restore_probe (replay_case_t::local_target);
}
TEST (DeferredJoinReservation, PrefixHandlersKeepIndependentJoinBoundary)
{
    restore_probe (replay_case_t::remote_with_barrier, true);
}
TEST (DeferredJoinReservation, RejectedDeferredBarrierReleasesBeforeReservation)
{
    restore_probe (replay_case_t::rejected_deferred);
}
void capacity_probe (bool handler_failure = false, bool extra_supply = false)
{
    auto mesh = fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("probe-mesh");
    auto node = mesh->spot_state;
    node->monitoring = fw::test::runtime_failure_monitoring ();
    auto scheduler = std::make_shared<fw::test::manual_internal_scheduler_t> ();
    node->worker_executor = scheduler;
    scheduler->publication_fence (
      fw::test::state_lane_publication_fence (node->lane, node->lane_executor));
    fw::serializer_registry_t serializers;
    fw::service_collection_t services;
    node->root_services = services.build_provider ();
    mesh->application_jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{
        fw::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    auto native = std::make_shared<rt::host::public_host_runtime_t> (rt::host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "probe-mesh",
                              .node_routing_id =
                                zlink::routing_id_t::from ("probe-source").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .application_jobs = mesh->application_jobs,
               .runtime_failures = node->runtime_failures ()},
      .object_stable_types = {"framework.spot", "probe"}});
    native->start ();
    node->native_node = native;
    auto held = mesh->application_jobs->try_reserve_supply ();
    ASSERT_TRUE (held);
    held->mark_queued ();

    auto spot = std::make_shared<detail::spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = fw::node_rid_t::from_string ("probe-source");
    spot->spot_id = fw::spot_id_t ("probe-source-spot");
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime = fw::test::runtime_failure_fixture<detail::channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, detail::spot_context_access_t::create (spot));
    detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.configure_instance = [] (void *, const fw::actor_ref_t &, void *) {};
    node->actor_factories.emplace ("probe", std::move (factory));
    node->actor_instances.emplace (actor_key, std::make_shared<int> (1));
    node->actor_spot_ids.emplace (actor_key, spot->spot_id);
    node->actor_generations.emplace (actor_key, 1);
    node->actor_types_by_id.emplace ("actor", "probe");
    const auto actor = detail::actor_ref_access_t::make (
      fw::node_rid_t::from_string ("probe-source"), "probe", "actor", 1);

    std::atomic<bool> first_started{false}, second_started{false};
    fw::task_completion_source_t<void> release_first;
    int join_activated = 0, join_cancelled = 0;
    std::shared_ptr<detail::deferred_barrier_t> failed_handler_join;
    std::mutex order_mutex;
    std::vector<std::string> order;
    std::shared_ptr<rt::application_job_queue_t::permit_t> ordinary_permit;
    const auto until = [&] (auto condition) {
        node->lane.run ([] {}).get ();
        scheduler->run_all ();
        scheduler->run_until (condition);
        return condition ();
    };
    spot->handlers.push_back (fw::spot_handler_descriptor_t{
      fw::spot_handler_kind_t::actor_send, packet_name, "", std::type_index (typeid (int)),
      std::type_index (typeid (void)), std::type_index (typeid (int)),
      std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&] (void *, void *, fw::service_provider_t &, fw::serializer_registry_t &,
           const zlink::message_t &,
           const fw::spot_inbound_message_t &metadata) -> fw::task_t<zlink::message_t> {
          const auto name = metadata.values.contains ("ordinary") ? "ordinary"
                            : metadata.values.contains ("second") ? "second"
                                                                  : "first";
          {
              std::lock_guard lock (order_mutex);
              order.push_back (name);
          }
          if (name == std::string_view ("first")) {
              if (handler_failure) {
                  auto reserved =
                    detail::spot_node_runtime_t (node).reserve_actor_join_barrier (actor);
                  if (!reserved)
                      throw fw::framework_exception_t (reserved.error_kind (),
                                                       "test Join reservation failed");
                  failed_handler_join = reserved.value ();
                  const auto registered = detail::defer_current_serial_turn (
                    [&] {
                        ++join_activated;
                        return fw::task_t<void> (fw::result_t<void>::success ());
                    },
                    [&] {
                        ++join_cancelled;
                        failed_handler_join->cancel ();
                    },
                    failed_handler_join);
                  if (!registered)
                      throw fw::framework_exception_t (registered.error_kind (),
                                                       "test Join registration failed");
              }
              first_started.store (true);
              co_await release_first.task ();
              if (handler_failure)
                  throw fw::framework_exception_t (fw::framework_error_kind_t::internal_failure,
                                                   "deterministic handler failure");
          } else if (name == std::string_view ("second")) {
              second_started.store (true);
          }
          co_return zlink::message_t{};
      });
    struct cleanup_t
    {
        std::function<void ()> action;
        ~cleanup_t ()
        {
            if (action)
                action ();
        }
    } cleanup{[&] {
        held.reset ();
        ordinary_permit.reset ();
        release_first.complete (fw::result_t<void>::success ());
        mesh->application_jobs->stop ();
        scheduler->run_all ();
        node->worker_executor->request_stop ();
        node->worker_executor->drain ();
        native->close ();
        node->native_node.reset ();
        (void) node->actor_transfer_coordinator->cancel_move (actor_key);
        node->spot_contexts_by_id.clear ();
        spot->node.reset ();
    }};
    if (handler_failure) {
        auto initial = std::make_shared<rt::application_job_queue_t::permit_t> (std::move (*held));
        held.reset ();
        auto record = accepted_record (mesh->application_jobs, 1);
        record->admission.retain_initial ([initial] { initial->release_for_handler_entry (); });
        auto failed = detail::spot_node_runtime_t (node).relay_actor_packet (
          actor, detail::actor_gateway_runtime_t{}.actor_context (actor),
          detail::stream_message_kind_t::send, packet_name, zlink::message_t::from ("{}"),
          *node->root_services, serializers, {}, nullptr, {}, {},
          zlink::routing_id_t::from ("probe-source"), {1, 1}, 0, std::nullopt, {}, 0, {}, record);
        scheduler->run_all ();
        const auto entered = first_started.load ();
        release_first.complete (fw::result_t<void>::success ());
        scheduler->run_until (failed);
        const auto activated = join_activated;
        const auto cancelled = join_cancelled;
        const auto failed_ready = failed.await_ready ();
        if (failed_handler_join)
            failed_handler_join->cancel ();
        scheduler->run_all ();
        EXPECT_TRUE (entered);
        EXPECT_EQ (activated, 0);
        EXPECT_EQ (cancelled, 1);
        EXPECT_TRUE (failed_ready);
        if (failed_ready) {
            EXPECT_FALSE (failed.result ());
            EXPECT_EQ (failed.result ().error_kind (),
                       fw::framework_error_kind_t::internal_failure);
        }
        EXPECT_EQ (mesh->application_jobs->snapshot ().permits_in_use, 0u);
        EXPECT_EQ (spot->ensure_spot_serial_executor ()->spot_queue ()->pending_count (), 0u);
        EXPECT_EQ (spot->ensure_spot_serial_executor ()
                     ->actor_executor (actor_key)
                     ->queue ()
                     ->pending_count (),
                   0u);
        std::fprintf (stdout,
                      "handler-task-failure entered=%d activated=%d cancelled=%d ready=%d\n",
                      entered, activated, cancelled, failed_ready);
        return;
    }
    ASSERT_TRUE (node->actor_transfer_coordinator->try_reserve_source (actor_key, first_transfer));
    ASSERT_TRUE (
      node->actor_transfer_coordinator->try_begin_source_remote (actor_key, first_transfer));
    for (const auto &metadata : {std::map<std::string, std::string>{},
                                 std::map<std::string, std::string>{{"second", "true"}}})
        ASSERT_EQ (
          detail::handoff_append_result_t::appended,
          node->actor_transfer_coordinator->try_append_backlog (
            actor_key, detail::handoff_packet_t{
                         packet_name,
                         {},
                         "application/json",
                         metadata,
                         false,
                         accepted_record (mesh->application_jobs, metadata.empty () ? 1 : 2)}));
    detail::spot_node_runtime_t (node).fail_remote_actor_transfer (actor, false, std::nullopt);
    ASSERT_TRUE (until ([&] {
        return first_started.load () || mesh->application_jobs->snapshot ().capacity_waiters == 1;
    }));
    // The held initial reservation must prevent the first replay callback.
    EXPECT_FALSE (first_started.load ());
    EXPECT_EQ (mesh->application_jobs->snapshot ().capacity_waiters, 1u);
    if (first_started.load ())
        return;
    held.reset ();
    ASSERT_TRUE (until ([&] { return first_started.load (); }));
    auto supply = mesh->application_jobs->try_reserve_supply ();
    ASSERT_TRUE (supply);
    ordinary_permit = std::make_shared<rt::application_job_queue_t::permit_t> (std::move (*supply));
    ordinary_permit->mark_queued ();
    auto ordinary = detail::spot_node_runtime_t (node).relay_actor_packet (
      actor, detail::actor_gateway_runtime_t{}.actor_context (actor),
      detail::stream_message_kind_t::send, packet_name, zlink::message_t::from ("{}"),
      *node->root_services, serializers,
      fw::spot_inbound_message_t{.values = {{"ordinary", "true"}}}, nullptr,
      [ordinary_permit] { ordinary_permit->release_for_handler_entry (); }, {},
      zlink::routing_id_t::from ("probe-source"), {1, 3}, 0, std::nullopt, {}, 0, {},
      accepted_record (mesh->application_jobs, 3));
    ASSERT_TRUE (until ([&] { return mesh->application_jobs->snapshot ().permits_in_use == 0; }));
    held = mesh->application_jobs->try_reserve_supply ();
    ASSERT_TRUE (held);
    held->mark_queued ();
    release_first.complete (fw::result_t<void>::success ());
    ASSERT_TRUE (until ([&] {
        return second_started.load () || mesh->application_jobs->snapshot ().capacity_waiters == 1;
    }));
    EXPECT_FALSE (second_started.load ());
    EXPECT_EQ (mesh->application_jobs->snapshot ().capacity_waiters, 1u);
    bool lifecycle_ran = false;
    rt::serial_work_options_t lifecycle{rt::serial_work_lane_t::lifecycle};
    ASSERT_TRUE (spot->ensure_spot_serial_executor ()->spot_queue ()->try_post (
      "permit-wait-lifecycle", [&] { lifecycle_ran = true; }, lifecycle));
    scheduler->run_all ();
    ASSERT_TRUE (lifecycle_ran);
    held.reset ();
    scheduler->run_all ();
    ASSERT_TRUE (ordinary.result ());
    spot->ensure_spot_serial_executor ()->actor_executor (actor_key)->queue ()->drain ();
    if (extra_supply) {
        auto extra = mesh->application_jobs->try_reserve_supply ();
        ASSERT_TRUE (extra);
        extra.reset ();
    }
    const auto final = mesh->application_jobs->snapshot ();
    EXPECT_EQ (final.peak_permits_in_use, 1u);
    EXPECT_EQ (final.permits_in_use, 0u);
    EXPECT_EQ (final.capacity_waiters, 0u);
    EXPECT_EQ (final.capacity_wait_count, 2u);
    // At capacity one with no cancellations, a supply acquisition either
    // starts a pause or fulfills a capacity wait without a state transition.
    const auto supply_acquisitions =
      mesh->application_jobs->pressure_metrics_snapshot ().paused_transition_count
      + final.capacity_wait_count;
    EXPECT_EQ (supply_acquisitions, 6u);
    std::lock_guard lock (order_mutex);
    EXPECT_EQ (order, (std::vector<std::string>{"first", "second", "ordinary"}));
}
TEST (DeferredJoinReservation, FailedHandlerTaskDiscardsDeferredJoin)
{
    capacity_probe (true);
}

TEST (DeferredJoinReservation, DurablePrefixTakesLateIngressPermitAtCapacityOne)
{
    capacity_probe ();
}

TEST (DeferredJoinReservation, DurablePrefixRejectsExtraSupplyAcquisition)
{
    EXPECT_NONFATAL_FAILURE (capacity_probe (false, true), "supply_acquisitions");
}

TEST (ActorMaintenanceJoin, CanonicalReplayMaterializesBodyWithoutInvocationCache)
{
    observations_t evidence;
    observations = &evidence;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, first_rid, std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                   .async<probe_t> ()
                   .result ());
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto actor =
      state->lane
        .run ([&] {
            return fw::detail::actor_ref_access_t::make (
              fw::node_rid_t::from_string (first_rid), actor_type, actor_id,
              state->actor_generations.at (std::string (actor_type) + ":" + actor_id));
        })
        .get ();
    namespace wire = fw::runtime::protocol;
    fw::runtime::messaging::envelope_header_t header;
    header.kind = fw::runtime::messaging::message_kind_t::command;
    header.channel_name = mesh_name;
    header.message_name = record_t::packet_name;
    header.content_type = "application/json";
    wire::application_payload_t::multipart_t parts;
    parts.push_back (fw::runtime::messaging::envelope_codec_t{}.encode_header (header));
    parts.push_back (zlink::message_t::from (nlohmann::json (record_t{73}).dump ()));
    wire::frozen_application_record_t frozen;
    frozen.kind = wire::frozen_record_kind_t::actor_send;
    frozen.body = wire::frozen_actor_application_body_t{
      {}, wire::application_payload_t::from_parts (std::move (parts))};
    auto record = std::make_shared<fw::runtime::serial_application_record_t> ();
    record->payload.emplace ();
    record->payload->application_record = std::move (frozen);
    int terminals = 0;
    record->native_reply = [&] (auto) {
        ++terminals;
        return true;
    };
    fw::detail::handoff_packet_t packet;
    packet.packet_name = record_t::packet_name;
    packet.content_type = header.content_type;
    packet.application_record = record;
    ASSERT_FALSE (record->invocation);
    ASSERT_TRUE (packet.payload.empty ());
    fw::detail::spot_node_runtime_t runtime (state);
    ASSERT_TRUE (fw::detail::spot_node_runtime_test_access_t::replay (
                   runtime, actor, {std::move (packet)}, services)
                   .result ());
    EXPECT_EQ (terminals, 1);
    EXPECT_EQ (evidence.recorded, (std::vector<int>{73}));
}

TEST (ActorMaintenanceJoin, MalformedCanonicalReplaySettlesNativeTerminal)
{
    observations_t evidence;
    observations = &evidence;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, first_rid, std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                   .async<probe_t> ()
                   .result ());
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto actor =
      state->lane
        .run ([&] {
            return fw::detail::actor_ref_access_t::make (
              fw::node_rid_t::from_string (first_rid), actor_type, actor_id,
              state->actor_generations.at (std::string (actor_type) + ":" + actor_id));
        })
        .get ();
    namespace wire = fw::runtime::protocol;
    fw::runtime::messaging::envelope_header_t header;
    header.kind = fw::runtime::messaging::message_kind_t::command;
    header.channel_name = mesh_name;
    header.message_name = record_t::packet_name;
    header.content_type = "application/json";
    wire::application_payload_t::multipart_t parts;
    parts.push_back (fw::runtime::messaging::envelope_codec_t{}.encode_header (header));
    wire::frozen_application_record_t frozen;
    frozen.kind = wire::frozen_record_kind_t::actor_send;
    frozen.body = wire::frozen_actor_application_body_t{
      {}, wire::application_payload_t::from_parts (std::move (parts))};
    auto record = std::make_shared<fw::runtime::serial_application_record_t> ();
    record->payload.emplace ();
    record->payload->application_record = std::move (frozen);
    int terminals = 0;
    record->native_reply = [&] (auto) {
        ++terminals;
        return true;
    };
    fw::detail::handoff_packet_t packet;
    packet.packet_name = record_t::packet_name;
    packet.content_type = header.content_type;
    packet.application_record = record;
    ASSERT_FALSE (record->invocation);
    ASSERT_TRUE (packet.payload.empty ());
    fw::detail::spot_node_runtime_t runtime (state);
    ASSERT_TRUE (fw::detail::spot_node_runtime_test_access_t::replay (
                   runtime, actor, {std::move (packet)}, services)
                   .result ());
    EXPECT_EQ (terminals, 1);
    EXPECT_TRUE (evidence.recorded.empty ());
}

}
