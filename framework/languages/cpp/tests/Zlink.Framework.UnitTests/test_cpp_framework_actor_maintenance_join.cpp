/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <zlink/framework.hpp>
#include "runtime/locations/in_memory_store_providers.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <condition_variable>
#include <thread>

namespace
{
namespace fw = zlink::framework;
using namespace std::chrono_literals;
constexpr auto wait_budget = 5s;
constexpr auto default_follow_duration = 30s;
constexpr auto source_follow_duration = 3s;
constexpr int instance_probe = -1;
constexpr auto actor_type = "member";
constexpr auto actor_id = "member";
constexpr auto mesh_name = "maintenance-join";
constexpr auto room_type = "room";
constexpr auto first_rid = "join-first";
constexpr auto second_rid = "join-second";
constexpr auto previous_spot = "previous";
constexpr auto maintenance_spot = "maintenance";
constexpr auto source_spot = "source";
constexpr auto local_spot = "local";
constexpr auto current_spot = "current";
struct observations_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<fw::actor_join_completion_t> completions;
    std::vector<std::string> completion_owners;
    bool leave_entered = false;
    int instances = 0;
    std::vector<int> destroyed;
    fw::task_completion_source_t<void> release_leave;
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
        {
            std::lock_guard lock (observations->mutex);
            observations->completions.push_back (completion);
            observations->completion_owners.push_back (
              std::string (_context.actor_ref ().node_rid ().value ()));
        }
        observations->changed.notify_all ();
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
        co_return std::make_shared<actor_t> (std::move (context));
    }
};
class entry_t final : public fw::entry_spot_t<actor_t>
{
  public:
    explicit entry_t (fw::entry_spot_context_t context) : _context (std::move (context)) {}
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override { _context.handlers ().add_actor_send<&entry_t::move> (); }
    void move (actor_t &actor, fw::message_context_t &, const move_t &request)
    {
        actor.context ().join_spot (fw::spot_id_t (request.target)).defer ();
    }
    fw::task_t<void> on_actor_joined (actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (actor_t &) override { co_return; }

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
        co_return _context.spot_id () == maintenance_spot ? fw::spot_actor_join_result_t::reject ()
                                                          : fw::spot_actor_join_result_t::accept ();
    }
    fw::task_t<void> on_actor_joined (actor_t &) override { co_return; }
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
// Release the application callback before host teardown, including assertion failure.
struct release_leave_t
{
    observations_t &e;
    ~release_leave_t () { e.release_leave.complete (fw::result_t<void>::success ()); }
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
void configure (host_t &host,
                const std::string &rid,
                std::shared_ptr<fw::location_store_t> store,
                std::shared_ptr<fw::relocation_store_t> relocations,
                std::chrono::milliseconds follow_duration = default_follow_duration,
                int spot_limit = 0)
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
      .listen ("tcp://127.0.0.1:0")
      .add_entry_spot<entry_t> ([] (fw::entry_spot_context_t context) {
          return std::make_shared<entry_t> (std::move (context));
      })
      .add_spot_factory<room_t> (
        room_type,
        [] (fw::spot_context_t context) { return std::make_shared<room_t> (std::move (context)); },
        [spot_limit] (auto &factory) {
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
    if (!sent)
        return false;
    std::unique_lock lock (evidence.mutex);
    return evidence.changed.wait_for (lock, wait_budget,
                                      [&] { return evidence.completions.size () >= count; });
}
bool peer_ready (fw::route_mesh_runtime_t &routes)
{
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (routes.snapshot (mesh_name).ready_peer_count == 0
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    return routes.snapshot (mesh_name).ready_peer_count > 0;
}
TEST (ActorMaintenanceJoin, RejectsWhilePreviousSourceLeaveIsPending)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_leave_t release{evidence};
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
    ASSERT_TRUE (move_to (*sender, evidence, current_spot, 2));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (1)))
      << (std::get_if<fw::actor_join_failed_t> (&evidence.completions.at (1))
            ? static_cast<int> (
                std::get<fw::actor_join_failed_t> (evidence.completions.at (1)).error_kind)
            : -1);
    sender = &second_services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (*sender, evidence, maintenance_spot, 3));
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
TEST (ActorMaintenanceJoin, SameNodeMembershipPreservesReturnedActorUntilSourceCleanup)
{
    observations_t evidence;
    observations = &evidence;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_leave_t release{evidence};
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
    ASSERT_TRUE (move_to (*sender, evidence, source_spot, 4));
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
