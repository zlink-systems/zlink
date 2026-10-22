/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
namespace fw = zlink::framework;
using namespace std::chrono_literals;
constexpr auto wait_budget = 5s;
constexpr auto mesh_name = "spotwide-relocation";
constexpr auto actor_type = "member";
constexpr auto room_type = "room";
constexpr auto room_id = "relocated-room";
constexpr auto source_rid = "spotwide-source";
constexpr auto target_rid = "spotwide-target";

struct enter_t
{
    static constexpr const char *packet_name = "spotwide-enter";
    std::string spot;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (enter_t, spot)
struct where_t
{
    static constexpr const char *packet_name = "spotwide-where";
    std::string spot;
    std::string node;
    std::string handler;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (where_t, spot, node, handler)

struct prefix_t
{
    static constexpr const char *packet_name = "spot-abort-prefix";
    int value;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (prefix_t, value)

struct joins_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t accepted = 0;
};
class member_t final : public fw::actor_t
{
  public:
    member_t (fw::actor_context_t context, joins_t &joins) :
        _context (std::move (context)), _joins (joins)
    {
    }
    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }
    fw::task_t<void> on_join_completed (const fw::actor_join_completion_t &completion) override
    {
        {
            std::lock_guard lock (_joins.mutex);
            if (std::holds_alternative<fw::actor_join_accepted_t> (completion))
                ++_joins.accepted;
        }
        _joins.changed.notify_all ();
        co_return;
    }

  private:
    fw::actor_context_t _context;
    joins_t &_joins;
};
class member_factory_t final : public fw::actor_factory_t<member_t>
{
  public:
    explicit member_factory_t (joins_t &joins) : _joins (joins) {}
    fw::task_t<std::shared_ptr<member_t>> create (fw::actor_context_t context,
                                                  std::stop_token) override
    {
        co_return std::make_shared<member_t> (std::move (context), _joins);
    }

  private:
    joins_t &_joins;
};
class entry_t final : public fw::entry_spot_t<member_t>
{
  public:
    explicit entry_t (fw::entry_spot_context_t context) : _context (std::move (context)) {}
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_actor_send<&entry_t::enter> ();
        _context.handlers ().add_actor_request<&entry_t::where> ();
    }
    void enter (member_t &actor, fw::message_context_t &, const enter_t &request)
    {
        actor.context ().join_spot (fw::spot_id_t (request.spot)).defer ();
    }
    where_t where (member_t &actor, fw::message_context_t &, const where_t &)
    {
        return {std::string (_context.spot_id ()),
                std::string (actor.context ().actor_ref ().node_rid ().value ()), "Entry"};
    }
    fw::task_t<fw::actor_create_response_t> on_create_actor (member_t &,
                                                             const fw::message_t &) override
    {
        co_return fw::actor_create_response_t::accept ();
    }
    fw::task_t<void> on_actor_joined (member_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (member_t &) override { co_return; }

  private:
    fw::entry_spot_context_t _context;
};
class room_t final : public fw::spot_t<member_t>
{
  public:
    explicit room_t (fw::spot_context_t context) : _context (std::move (context)) {}
    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_actor_request<&room_t::where> ();
        _context.handlers ().add_handler<&room_t::prefix> ();
    }
    void prefix (fw::message_context_t &, const prefix_t &message)
    {
        if (received)
            received (message.value);
    }
    std::function<void (int)> received;
    std::function<fw::task_t<std::vector<std::byte>> ()> capture;

    where_t where (member_t &actor, fw::message_context_t &, const where_t &)
    {
        return {std::string (_context.spot_id ()),
                std::string (actor.context ().actor_ref ().node_rid ().value ()), room_type};
    }
    fw::task_t<fw::spot_create_response_t> on_create (const fw::message_t &) override
    {
        co_return fw::spot_create_response_t::accept ();
    }
    fw::task_t<void> on_initialize () override { co_return; }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::accept ();
    }
    fw::task_t<void> on_actor_joined (member_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (member_t &) override { co_return; }

  private:
    fw::spot_context_t _context;
};
class failing_capture_t final : public fw::spot_relocation_adapter_t<room_t>
{
  public:
    fw::task_t<std::vector<std::byte>> capture (room_t &spot, std::stop_token) override
    {
        co_return co_await spot.capture ();
    }
    fw::task_t<void> restore (room_t &, std::vector<std::byte>, std::stop_token) override
    {
        co_return;
    }
};
struct host_t
{
    fw::app_t app = fw::app_t::create ();
    std::thread worker;
    ~host_t ()
    {
        app.request_stop ();
        if (worker.joinable ())
            worker.join ();
    }
    void start ()
    {
        worker = std::thread ([this] {
            char name[] = "spotwide-relocation";
            char *argv[]{name, nullptr};
            app.run (1, argv);
        });
    }
};
template <class Predicate> bool observe (Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!predicate () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    return predicate ();
}

TEST (SpotWideHostRelocation, FrameworkManagedMembersKeepUserSpotHandler)
{
    joins_t joins;
    auto locations = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    const auto configure = [&] (host_t &host, const char *rid) {
        auto &options = host.app.add_zlink_framework ();
        options.add_location_store (locations);
        options.add_relocation_store (relocations);
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::normal);
        host.app.logging ().use_file (std::string (rid) + ".flow");
        auto mesh = options.add_route_mesh (mesh_name);
        mesh.set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (rid))
          .listen ("tcp://127.0.0.1:0")
          .add_entry_spot<entry_t> ([] (fw::entry_spot_context_t context) {
              return std::make_shared<entry_t> (std::move (context));
          })
          .add_actor_factory<member_t, member_factory_t> (
            actor_type, std::make_shared<member_factory_t> (joins),
            [] (auto &factory) { factory.recreate_on_relocation (); })
          .add_spot_factory<room_t> (
            room_type,
            [] (fw::spot_context_t context) {
                return std::make_shared<room_t> (std::move (context));
            },
            [] (auto &factory) {
                // Keep the public defaults: SpotWide and framework_managed.
                factory.recreate_on_relocation ();
            });
    };
    host_t source, target;
    configure (source, source_rid);
    source.start ();
    ASSERT_TRUE (observe ([&] { return source.app.is_ready (); }));
    auto services = source.app.advanced ().services ().build_provider ();
    auto &actors = services.get_required<fw::actor_manager_t> ();
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (room_id), room_type)
                   .async ()
                   .result ());
    for (const auto *id : {"wide-member-1", "wide-member-2", "entry-member"})
        ASSERT_TRUE (actors.get_or_create (fw::actor_id_t (id), actor_type).async ().result ());
    for (const auto *id : {"wide-member-1", "wide-member-2"})
        ASSERT_TRUE (client.send (fw::actor_id_t (id), enter_t{room_id}).async ().result ());
    {
        std::unique_lock lock (joins.mutex);
        ASSERT_TRUE (
          joins.changed.wait_for (lock, wait_budget, [&] { return joins.accepted == 2; }));
    }
    for (const auto *id : {"wide-member-1", "wide-member-2"}) {
        const auto before = client.request (fw::actor_id_t (id), where_t{})
                              .timeout (wait_budget)
                              .async<where_t> ()
                              .result ();
        ASSERT_TRUE (before);
        ASSERT_EQ (before.value ().spot, room_id);
        ASSERT_EQ (before.value ().handler, room_type);
        ASSERT_EQ (before.value ().node, source_rid);
    }
    configure (target, target_rid);
    target.start ();
    ASSERT_TRUE (observe ([&] { return target.app.is_ready (); }));
    auto &routes = services.get_required<fw::route_mesh_runtime_t> ();
    ASSERT_TRUE (observe ([&] { return routes.snapshot (mesh_name).ready_peer_count == 1; }));
    const auto moved =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result ();
    ASSERT_TRUE (moved);
    ASSERT_EQ (moved.value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (moved.value ().reason);
    auto target_services = target.app.advanced ().services ().build_provider ();
    auto &target_client = target_services.get_required<fw::actor_client_t> ();
    for (const auto *id : {"wide-member-1", "wide-member-2", "entry-member"}) {
        const auto after = target_client.request (fw::actor_id_t (id), where_t{})
                             .timeout (wait_budget)
                             .async<where_t> ()
                             .result ();
        ASSERT_TRUE (after) << id;
        EXPECT_EQ (after.value ().node, target_rid) << id;
        if (std::string_view (id) == "entry-member") {
            EXPECT_EQ (after.value ().handler, "Entry");
            EXPECT_NE (after.value ().spot, room_id);
        } else {
            EXPECT_EQ (after.value ().handler, room_type) << id;
            EXPECT_EQ (after.value ().spot, room_id) << id;
        }
    }
    std::lock_guard lock (joins.mutex);
    EXPECT_EQ (joins.accepted, 2u);
}
TEST (SpotWideHostRelocation, AbortRunsAcceptedSpotPrefixWithoutNewIngress)
{
    auto locations = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    fw::task_completion_source_t<void> capturing, release;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<int> executed;
    const auto capture = [&] () -> fw::task_t<std::vector<std::byte>> {
        capturing.complete (fw::result_t<void>::success ());
        co_await release.task ();
        throw std::runtime_error ("intentional source capture failure");
        co_return std::vector<std::byte>{};
    };
    const auto configure = [&] (host_t &host, const char *rid) {
        auto &options = host.app.add_zlink_framework ();
        options.add_location_store (locations);
        options.add_relocation_store (relocations);
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
        host.app.logging ().use_file (std::string (rid) + "-abort.flow");
        options.add_route_mesh (mesh_name)
          .set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (rid))
          .listen ("tcp://127.0.0.1:0")
          .add_spot_factory<room_t> (
            room_type,
            [&] (fw::spot_context_t context) {
                auto spot = std::make_shared<room_t> (std::move (context));
                spot->capture = capture;
                spot->received = [&] (int value) {
                    std::lock_guard lock (mutex);
                    executed.push_back (value);
                    changed.notify_all ();
                };
                return spot;
            },
            [] (auto &factory) { factory.template preserve_state_with<failing_capture_t> (); });
    };
    host_t source, target;
    struct release_capture_t
    {
        fw::task_completion_source_t<void> &release;
        ~release_capture_t ()
        {
            if (!release.task ().await_ready ())
                release.complete (fw::result_t<void>::success ());
        }
    } release_capture{release};
    configure (source, "abort-source");
    configure (target, "abort-target");
    source.start ();
    target.start ();
    ASSERT_TRUE (observe ([&] { return source.app.is_ready () && target.app.is_ready (); }));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (observe ([&] {
        return services.get_required<fw::route_mesh_runtime_t> ()
                 .snapshot (mesh_name)
                 .ready_peer_count
               == 1;
    }));
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (room_id), room_type)
                   .async ()
                   .result ());
    const auto registration =
      fw::detail::mesh_node_runtime_t::registrations (source.app.advanced ().zlink ()).front ();
    const auto gate = fw::detail::spot_node_runtime_t (registration->spot_state)
                        .spot_execution_queue (room_id)
                        .result ()
                        .value ();
    ASSERT_TRUE (gate);
    gate->set_application_start_policy ([] (const auto &) { return false; });
    auto client = source.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    for (int value : {1, 2}) {
        const auto sent =
          client.send_to_spot (fw::spot_id_t (room_id), prefix_t{value}).async ().result ();
        ASSERT_TRUE (sent) << static_cast<int> (sent.error_kind ()) << " "
                           << (sent.error () ? sent.error ()->what () : "");
    }
    ASSERT_TRUE (observe (
      [&] { return gate->pending_count (fw::runtime::serial_work_lane_t::application) == 2; }));
    auto moving = source.app.relocate (
      {.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget});
    ASSERT_TRUE (observe ([&] { return capturing.task ().await_ready (); }));
    gate->set_application_start_policy ([] (const auto &) { return true; });
    release.complete (fw::result_t<void>::success ());
    const auto result = moving.result ();
    ASSERT_TRUE (result);
    EXPECT_EQ (result.value ().outcome, fw::relocation_outcome_t::blocked);
    EXPECT_EQ (result.value ().reason, fw::relocation_reason_t::relocation_failed);
    std::unique_lock lock (mutex);
    ASSERT_TRUE (changed.wait_for (lock, wait_budget, [&] { return executed.size () == 2; }));
    EXPECT_EQ (executed, (std::vector<int>{1, 2}));
}
} // namespace
