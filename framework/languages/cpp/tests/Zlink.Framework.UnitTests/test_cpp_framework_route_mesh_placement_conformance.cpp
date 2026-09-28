/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// Consumes framework/runtime/conformance/route-mesh-placement-v1.json through the public host:
// RouteMesh placement counts come from the reporting MeshNode's activation records, and
// IsAvailable follows runtime monitoring §5 and MeshNode §5.1.

#include <zlink/framework.hpp>
#include "runtime/locations/in_memory_store_providers.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef ZLINK_ROUTE_MESH_PLACEMENT_CONFORMANCE_PATH
#error "RouteMesh placement conformance fixture path is required"
#endif

namespace
{
namespace fw = zlink::framework;

// Keeps a fixture operation in flight (`hold`) until the scenario's expectations are checked.
class hold_t
{
  public:
    void reset (std::set<std::string> kinds)
    {
        std::lock_guard lock (_mutex);
        _kinds = std::move (kinds);
        _entered = 0;
        _released = false;
    }

    void wait_if (const std::string &kind)
    {
        std::unique_lock lock (_mutex);
        if (_released || !_kinds.contains (kind))
            return;
        ++_entered;
        _changed.notify_all ();
        _changed.wait (lock, [&] { return _released; });
    }

    bool wait_entered (int count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, timeout, [&] { return _entered >= count; });
    }

    void release ()
    {
        {
            std::lock_guard lock (_mutex);
            _released = true;
        }
        _changed.notify_all ();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::set<std::string> _kinds;
    int _entered = 0;
    bool _released = false;
};

hold_t hold;
// The User Spot an Actor joins right after its creation (fixture object `actorJoin`).
std::optional<std::string> join_spot_id;
std::string join_failure;

class placement_actor_t final : public fw::actor_t
{
  public:
    explicit placement_actor_t (fw::actor_context_t context) : _context (std::move (context)) {}
    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }

  private:
    fw::actor_context_t _context;
};

class placement_actor_factory_t final : public fw::actor_factory_t<placement_actor_t>
{
  public:
    fw::task_t<std::shared_ptr<placement_actor_t>> create (fw::actor_context_t context,
                                                           std::stop_token) override
    {
        hold.wait_if ("actor");
        co_return std::make_shared<placement_actor_t> (std::move (context));
    }
};

struct placement_probe_message_t
{
    static constexpr const char *packet_name = "placement.actor.probe";
    int value = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (placement_probe_message_t, value)

class placement_entry_spot_t final : public fw::entry_spot_t<placement_actor_t>
{
  public:
    explicit placement_entry_spot_t (fw::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_actor_send<&placement_entry_spot_t::on_probe> ();
    }
    // An Actor joins another Spot only from its own handler turn, so the fixture's `actorJoin`
    // starts from a probe message the Actor handles in its Entry Spot.
    fw::task_t<void>
    on_probe (placement_actor_t &actor, fw::message_context_t &, const placement_probe_message_t &)
    {
        if (join_spot_id) {
            try {
                actor.context ().join_spot (fw::spot_id_t (*join_spot_id)).defer ();
            }
            catch (const std::exception &error) {
                join_failure = error.what ();
            }
        }
        co_return;
    }
    fw::task_t<void> on_actor_joined (placement_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (placement_actor_t &) override { co_return; }

  private:
    fw::entry_spot_context_t _context;
};

class placement_spot_t final : public fw::spot_t<placement_actor_t>
{
  public:
    explicit placement_spot_t (fw::spot_context_t context) : _context (std::move (context)) {}
    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    fw::task_t<fw::spot_create_response_t> on_create (const fw::message_t &) override
    {
        hold.wait_if ("userSpot");
        co_return fw::spot_create_response_t::accept ();
    }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fw::message_t &) override
    {
        hold.wait_if ("actorJoin");
        co_return fw::spot_actor_join_result_t::accept ();
    }
    fw::task_t<void> on_actor_joined (placement_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (placement_actor_t &) override { co_return; }

  private:
    fw::spot_context_t _context;
};

struct placement_instance_probe_t
{
    static constexpr const char *packet_name = "placement.instance.probe";
    int value = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (placement_instance_probe_t, value)

class placement_instance_spot_t final : public fw::instance_spot_t
{
  public:
    explicit placement_instance_spot_t (fw::instance_spot_context_t context) :
        _context (std::move (context))
    {
    }
    fw::instance_spot_context_t &context () noexcept override { return _context; }
    const fw::instance_spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_handler<&placement_instance_spot_t::on_probe> ();
    }
    void on_probe (const placement_instance_probe_t &) {}

  private:
    fw::instance_spot_context_t _context;
};

bool wait_for_route_status (fw::route_mesh_runtime_t &routes,
                            const std::string &mesh_name,
                            const std::function<bool ()> &condition,
                            std::chrono::milliseconds timeout)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool signaled = false;
    auto observation = routes.observe (mesh_name, 8, [&] (const auto &) {
        {
            std::lock_guard lock (mutex);
            signaled = true;
        }
        changed.notify_one ();
    });
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    std::unique_lock lock (mutex);
    bool matched = false;
    while (true) {
        lock.unlock ();
        matched = condition ();
        lock.lock ();
        if (matched)
            break;
        if (!changed.wait_until (lock, deadline, [&] { return signaled; }))
            break;
        signaled = false;
    }
    lock.unlock ();
    observation->close ();
    return matched || condition ();
}

std::string spot_type (const std::string &mesh_name)
{
    return "placement-spot-" + mesh_name;
}

std::string instance_type (const std::string &mesh_name)
{
    return "placement-instance-" + mesh_name;
}

std::string reason_name (const std::optional<fw::topology_reason_t> &reason)
{
    if (!reason)
        return "null";
    switch (*reason) {
        case fw::topology_reason_t::runtime_not_ready:
            return "runtime_not_ready";
        case fw::topology_reason_t::no_ready_peer:
            return "no_ready_peer";
        case fw::topology_reason_t::no_ready_target:
            return "no_ready_target";
        case fw::topology_reason_t::location_unavailable:
            return "location_unavailable";
        case fw::topology_reason_t::capacity_exceeded:
            return "capacity_exceeded";
        case fw::topology_reason_t::draining:
            return "draining";
        case fw::topology_reason_t::internal_failure:
            return "internal_failure";
    }
    return "unknown";
}

struct host_t
{
    fw::app_t app = fw::app_t::create ();
    std::shared_ptr<fw::runtime::in_memory_location_store_t> store =
      std::make_shared<fw::runtime::in_memory_location_store_t> ();
    int exit_code = -1;
    std::thread thread;
};

bool wait_for_host_ready (host_t &host, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (host.app.is_ready ())
            return true;
        std::this_thread::yield ();
    }
    return host.app.is_ready ();
}

void configure_host (host_t &host,
                     const nlohmann::json &lease,
                     const nlohmann::json &mesh_nodes,
                     const std::string &prefix,
                     std::optional<int> startup_weight = std::nullopt)
{
    auto &options = host.app.add_zlink_framework ();
    options.add_location_store (host.store);
    options.add_relocation_store (std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    auto &locations = options.configure_locations ();
    locations.owner_lease_renew_interval =
      std::chrono::milliseconds (lease.at ("renewIntervalMs").get<int> ());
    locations.owner_lease_renew_timeout =
      std::chrono::milliseconds (lease.at ("renewTimeoutMs").get<int> ());
    locations.owner_lease_ttl = std::chrono::milliseconds (lease.at ("ttlMs").get<int> ());
    locations.owner_lease_fencing_margin =
      std::chrono::milliseconds (lease.at ("fencingMarginMs").get<int> ());
    locations.polling_interval = std::chrono::milliseconds (10);
    for (const auto &node : mesh_nodes) {
        const auto mesh_name = node.at ("meshName").get<std::string> ();
        auto mesh = options.add_route_mesh (mesh_name);
        mesh.set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (prefix + "-" + mesh_name))
          .listen ("inproc://" + prefix + "-" + mesh_name)
          .set_actor_limit (node.at ("actorLimit").get<std::int32_t> ())
          .set_spot_limit (node.at ("spotLimit").get<std::int32_t> ())
          .set_activation_concurrency (node.at ("activationConcurrency").get<std::int32_t> ())
          .add_spot_factory<placement_spot_t> (
            spot_type (mesh_name),
            [] (fw::spot_context_t context) {
                return std::make_shared<placement_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); });
        if (startup_weight)
            mesh.set_placement_weight (*startup_weight);
        if (node.at ("actorFactory").get<bool> ()) {
            mesh
              .add_entry_spot<placement_entry_spot_t> ([] (fw::entry_spot_context_t context) {
                  return std::make_shared<placement_entry_spot_t> (std::move (context));
              })
              .add_actor_factory<placement_actor_t, placement_actor_factory_t> (
                "placement-actor", std::make_shared<placement_actor_factory_t> (),
                [] (auto &factory) { factory.disable_relocation (); });
        }
        if (node.at ("instanceSpotFactory").get<bool> ()) {
            mesh.add_instance_spot_factory<placement_instance_spot_t> (
              instance_type (mesh_name),
              [] (fw::instance_spot_context_t context) {
                  hold.wait_if ("instanceSpot");
                  return std::make_shared<placement_instance_spot_t> (std::move (context));
              },
              [] (auto &factory) { factory.disable_relocation (); });
        }
    }
}

void start_host (host_t &host)
{
    host.thread = std::thread ([&host] {
        char program[] = "route-mesh-placement-conformance";
        char *arguments[] = {program, nullptr};
        host.exit_code = host.app.run (1, arguments);
    });
}

void stop_host (host_t &host)
{
    host.app.request_stop ();
    if (host.thread.joinable ())
        host.thread.join ();
}

bool run_scenario (const nlohmann::json &lease, const nlohmann::json &scenario, int index)
{
    const auto name = scenario.at ("name").get<std::string> ();
    for (const auto &node : scenario.at ("meshNodes")) {
        if (node.at ("spotRelocation") != "disabled") {
            std::cerr << "[placement] " << name << ": unsupported spotRelocation\n";
            return false;
        }
    }
    std::set<std::string> held_kinds;
    int held_count = 0;
    bool has_join = false;
    for (const auto &objects : scenario.at ("objects")) {
        has_join = has_join || objects.at ("kind") == "actorJoin";
        if (objects.at ("hold").get<bool> ()) {
            held_kinds.insert (objects.at ("kind").get<std::string> ());
            held_count += objects.at ("count").get<int> ();
        }
    }
    hold.reset (held_kinds);
    join_spot_id.reset ();
    join_failure.clear ();

    host_t host;
    configure_host (host, lease, scenario.at ("meshNodes"), "placement-" + std::to_string (index));
    start_host (host);
    bool ok = wait_for_host_ready (host, std::chrono::seconds (5));
    if (!ok)
        std::cerr << "[placement] " << name
                  << ": host never reached Serving, exit=" << host.exit_code << '\n';

    auto services = host.app.advanced ().services ().build_provider ();
    auto spots = services.get_required<fw::spot_manager_t> ();
    auto &actors = services.get_required<fw::actor_manager_t> ();
    auto &routes = services.get_required<fw::route_mesh_runtime_t> ();
    auto route_client = services.get_required<fw::route_client_t> ();
    // Creation is admitted once the host holds its owner lease (Location runtime §5).
    for (const auto &node : scenario.at ("meshNodes")) {
        const auto mesh_name = node.at ("meshName").get<std::string> ();
        ok =
          ok
          && wait_for_route_status (
            routes, mesh_name, [&] { return routes.snapshot (mesh_name).placement.is_available; },
            std::chrono::seconds (5));
    }

    int object_index = 0;
    std::optional<std::string> join_actor_id;
    std::string last_error;
    std::vector<std::thread> held_operations;
    const auto create_actor = [&] (const std::string &mesh_name, fw::actor_id_t actor_id) {
        auto created = actors.create (actor_id, "placement-actor")
                         .in_mesh (mesh_name)
                         .timeout (std::chrono::seconds (5))
                         .async ()
                         .result ();
        if (!created && created.error ())
            last_error = created.error ()->what ();
        return static_cast<bool> (created)
               && std::holds_alternative<fw::actor_create_created_t> (created.value ());
    };
    const auto create_spot = [&] (const std::string &mesh_name) -> std::optional<std::string> {
        auto created = spots.create (spot_type (mesh_name))
                         .in_mesh (mesh_name)
                         .timeout (std::chrono::seconds (5))
                         .async ()
                         .result ();
        if (!created && created.error ())
            last_error = created.error ()->what ();
        if (!created)
            return std::nullopt;
        return std::string (created.value ().spot.spot_id ());
    };
    const auto activate_instance = [&] (const std::string &mesh_name, std::string spot_id) {
        auto sent =
          route_client.send_to_spot (fw::spot_id_t (spot_id), placement_instance_probe_t{1})
            .instance_spot (instance_type (mesh_name))
            .in_mesh (mesh_name)
            .async ()
            .result ();
        if (!sent && sent.error ())
            last_error = sent.error ()->what ();
        return static_cast<bool> (sent);
    };

    // An Actor joins a Spot from its own handler turn, so the Spot it joins exists first.
    auto objects = scenario.at ("objects");
    if (has_join) {
        std::stable_sort (
          objects.begin (), objects.end (), [] (const auto &left, const auto &right) {
              return left.at ("kind") == "userSpot" && right.at ("kind") != "userSpot";
          });
    }
    for (const auto &entry : objects) {
        if (!ok)
            break;
        const auto kind = entry.at ("kind").get<std::string> ();
        const auto mesh_name = entry.at ("meshName").get<std::string> ();
        const auto held = entry.at ("hold").get<bool> ();
        for (int count = 0; ok && count < entry.at ("count").get<int> (); ++count) {
            const auto object_name = name + "-" + std::to_string (object_index++);
            if (kind == "actorJoin")
                continue;
            if (held) {
                held_operations.emplace_back ([&, kind, mesh_name, object_name] {
                    if (kind == "actor")
                        (void) create_actor (mesh_name, fw::actor_id_t (object_name));
                    else if (kind == "userSpot")
                        (void) create_spot (mesh_name);
                    else
                        (void) activate_instance (mesh_name, object_name);
                });
                continue;
            }
            if (kind == "actor") {
                ok = create_actor (mesh_name, fw::actor_id_t (object_name));
                if (ok)
                    join_actor_id = object_name;
            } else if (kind == "userSpot") {
                const auto created = create_spot (mesh_name);
                ok = created.has_value ();
                if (created && has_join)
                    join_spot_id = *created;
            } else {
                ok = activate_instance (mesh_name, object_name);
            }
            if (!ok)
                std::cerr << "[placement] " << name << ": " << kind << " on " << mesh_name
                          << " failed: " << last_error << '\n';
        }
    }
    if (ok && has_join && join_actor_id) {
        auto sent = services.get_required<fw::actor_client_t> ()
                      .send (fw::actor_id_t (*join_actor_id), placement_probe_message_t{1})
                      .async ()
                      .result ();
        if (!sent) {
            join_failure = sent.error () ? sent.error ()->what () : "probe send failed";
            ok = false;
        }
    }
    if (ok && held_count > 0 && !hold.wait_entered (held_count, std::chrono::seconds (5))) {
        std::cerr << "[placement] " << name << ": held operation never started " << last_error
                  << join_failure << '\n';
        ok = false;
    }

    auto &runtime_options = services.get_required<fw::route_mesh_runtime_options_t> ();
    for (const auto &node : scenario.at ("meshNodes"))
        runtime_options.mesh (node.at ("meshName").get<std::string> ())
          .placement_weight (node.at ("placementWeightAfterStartup").get<int> ());
    for (const auto &expected : scenario.at ("expected")) {
        if (!ok)
            break;
        const auto mesh_name = expected.at ("meshName").get<std::string> ();
        const auto expected_state = expected.at ("state") == "degraded"
                                      ? fw::mesh_node_state_t::degraded
                                      : fw::mesh_node_state_t::ready;
        const auto expected_available = expected.at ("isAvailable").get<bool> ();
        const auto matches = [&] (const fw::mesh_node_snapshot_t &status) {
            return status.placement.active_actor_count
                     == expected.at ("activeActorCount").get<std::uint32_t> ()
                   && status.placement.active_spot_count
                        == expected.at ("activeSpotCount").get<std::uint32_t> ()
                   && status.placement.is_available == expected_available
                   && status.state == expected_state;
        };
        fw::mesh_node_snapshot_t status;
        wait_for_route_status (
          routes, mesh_name,
          [&] {
              status = routes.snapshot (mesh_name);
              return matches (status);
          },
          std::chrono::seconds (5));
        if (!matches (status)) {
            std::fprintf (stderr,
                          "[placement] %s:%s actors=%u spots=%u available=%d reason=%s "
                          "degraded=%d\n",
                          name.c_str (), mesh_name.c_str (), status.placement.active_actor_count,
                          status.placement.active_spot_count, status.placement.is_available ? 1 : 0,
                          reason_name (status.placement.unavailable_reason).c_str (),
                          status.state == fw::mesh_node_state_t::degraded ? 1 : 0);
            ok = false;
        }
    }

    hold.release ();
    for (auto &operation : held_operations)
        operation.join ();
    if (!join_failure.empty ()) {
        std::cerr << "[placement] " << name << ": Actor join failed: " << join_failure << '\n';
        ok = false;
    }
    join_spot_id.reset ();
    stop_host (host);
    return ok;
}

// MeshNode §5.1: placement weight 0 excludes the MeshNode only from new target selection, so a
// host whose MeshNode starts with weight 0 still starts, and a new placement still excludes it.
bool zero_weight_at_startup (const nlohmann::json &lease)
{
    hold.reset ({});
    const auto mesh_nodes = nlohmann::json::array ({{{"meshName", "zero"},
                                                     {"actorLimit", 0},
                                                     {"spotLimit", 0},
                                                     {"activationConcurrency", 128},
                                                     {"actorFactory", true},
                                                     {"instanceSpotFactory", false}}});
    host_t host;
    configure_host (host, lease, mesh_nodes, "placement-zero-weight", 0);
    start_host (host);
    bool ok = wait_for_host_ready (host, std::chrono::seconds (5));
    if (!ok) {
        std::cerr << "[placement] zero-weight-at-startup: host never reached Serving, exit="
                  << host.exit_code << '\n';
        stop_host (host);
        return false;
    }
    auto services = host.app.advanced ().services ().build_provider ();
    auto &routes = services.get_required<fw::route_mesh_runtime_t> ();
    ok = wait_for_route_status (
      routes, "zero",
      [&] {
          const auto status = routes.snapshot ("zero");
          return status.state == fw::mesh_node_state_t::ready && !status.placement.is_available;
      },
      std::chrono::seconds (5));
    auto created = services.get_required<fw::spot_manager_t> ()
                     .create (spot_type ("zero"))
                     .in_mesh ("zero")
                     .timeout (std::chrono::seconds (1))
                     .async ()
                     .result ();
    if (!ok || created || created.error_kind () != fw::framework_error_kind_t::unavailable) {
        std::cerr << "[placement] zero-weight-at-startup: status or new placement mismatch\n";
        ok = false;
    }
    stop_host (host);
    return ok;
}

} // namespace

int main ()
{
    std::ifstream input (ZLINK_ROUTE_MESH_PLACEMENT_CONFORMANCE_PATH);
    if (!input) {
        std::cerr << "RouteMesh placement conformance fixture could not be opened\n";
        return 1;
    }
    const auto fixture = nlohmann::json::parse (input);
    if (fixture.at ("fixture") != "zlink.framework.route-mesh-placement"
        || fixture.at ("version") != 1)
        return 1;
    int index = 0;
    int failures = 0;
    int scenarios = 0;
    for (const auto &scenario : fixture.at ("scenarios")) {
        ++scenarios;
        failures += run_scenario (fixture.at ("ownerLease"), scenario, index++) ? 0 : 1;
    }
    std::cout << "[placement] " << scenarios - failures << "/" << scenarios
              << " scenarios passed\n";
    if (!zero_weight_at_startup (fixture.at ("ownerLease")))
        ++failures;
    return failures == 0 ? 0 : 1;
}
