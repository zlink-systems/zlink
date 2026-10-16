/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>

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
constexpr auto failed_spot = "failed";
constexpr auto factory_failed_spot = "factory-failed";
constexpr auto framework_failed_spot = "framework-failed";
constexpr auto rejected_spot = "rejected";
constexpr auto rejection_reply = "room is full";
struct observations_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<fw::actor_join_completion_t> completions;
    std::vector<std::string> completion_owners;
    bool leave_entered = false;
    bool materialization_entered = false;
    int instances = 0;
    bool fail_remote_factory = false;
    int remote_factory_failure_count = 0;
    std::vector<int> destroyed;
    std::vector<std::pair<fw::spot_close_reason_t, std::size_t>> closings;
    int destroy_callbacks = 0;
    int entry_joined = 0;
    int entry_left = 0;
    fw::task_completion_source_t<void> release_leave;
    std::optional<std::size_t> completion_to_hold;
    fw::task_completion_source_t<void> release_completion;
    std::vector<std::string> factory_nodes;
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
        {
            std::lock_guard lock (observations->mutex);
            observations->completions.push_back (completion);
            observations->completion_owners.push_back (
              std::string (_context.actor_ref ().node_rid ().value ()));
            hold = observations->completion_to_hold == observations->completions.size ();
        }
        observations->changed.notify_all ();
        if (hold)
            co_await observations->release_completion.task ();
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
    }
    probe_t probe (actor_t &actor, fw::message_context_t &, const probe_t &request)
    {
        return request.value == instance_probe ? probe_t{actor.instance} : request;
    }
    void move (actor_t &actor, fw::message_context_t &, const move_t &request)
    {
        actor.context ().join_spot (fw::spot_id_t (request.target)).defer ();
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
        _context.handlers ().add_handler<&room_t::ready> ();
    }
    fw::task_t<void> ready (const ready_t &)
    {
        _context.relocation_ready ().defer ();
        co_return;
    }
    void move (actor_t &actor, fw::message_context_t &, const move_t &request)
    {
        actor.context ().join_spot (fw::spot_id_t (request.target)).defer ();
    }
    probe_t probe (actor_t &actor, fw::message_context_t &, const probe_t &request)
    {
        return request.value == instance_probe ? probe_t{actor.instance} : request;
    }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fw::message_t &) override
    {
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
    fw::task_t<void> on_actor_joined (actor_t &) override { co_return; }
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
        e.release_leave.complete (fw::result_t<void>::success ());
        e.release_completion.complete (fw::result_t<void>::success ());
        std::lock_guard lock (e.mutex);
        e.changed.notify_all ();
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
bool peer_ready (fw::route_mesh_runtime_t &routes, std::size_t count = 1)
{
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (routes.snapshot (mesh_name).ready_peer_count < count
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    return routes.snapshot (mesh_name).ready_peer_count >= count;
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
    evidence.fail_remote_factory =
      std::string_view (std::get<1> (GetParam ())) == factory_failed_spot;
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
    auto probe = client.request (fw::actor_id_t (actor_id), probe_t{17})
                   .timeout (wait_budget)
                   .async<probe_t> ()
                   .result ();
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
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (),
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
} // namespace
