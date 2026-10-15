/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
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
    int instances = 0;
    bool fail_remote_factory = false;
    int remote_factory_failure_count = 0;
    std::vector<int> destroyed;
    fw::task_completion_source_t<void> release_leave;
    std::optional<std::size_t> completion_to_hold;
    fw::task_completion_source_t<void> release_completion;
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
    probe_t probe (actor_t &, fw::message_context_t &, const probe_t &request) { return request; }
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
// Release application callbacks before host teardown, including assertion failure.
struct release_callbacks_t
{
    observations_t &e;
    ~release_callbacks_t ()
    {
        e.release_leave.complete (fw::result_t<void>::success ());
        e.release_completion.complete (fw::result_t<void>::success ());
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
bool peer_ready (fw::route_mesh_runtime_t &routes)
{
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (routes.snapshot (mesh_name).ready_peer_count == 0
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    return routes.snapshot (mesh_name).ready_peer_count > 0;
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
    ASSERT_TRUE (probe);
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
