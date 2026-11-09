/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/client_server/raw_client_server_owner.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <filesystem>
#include <iostream>
#include <type_traits>
#include <future>
#include <chrono>
namespace fw = zlink::framework;
using owner_t = fw::runtime::client_server::raw_client_server_server_t;
using options_t = fw::runtime::client_server::raw_client_server_server_options_t;
std::size_t threads ()
{
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator ("/proc/self/task")) {
        (void) entry;
        ++count;
    }
    return count;
}
template <class Owner>
auto make_owner (options_t options,
                 std::shared_ptr<zlink::context_t> context,
                 fw::runtime::offload_executor_t &executor)
{
    if constexpr (std::is_constructible_v<Owner, options_t, std::shared_ptr<zlink::context_t>,
                                          fw::runtime::offload_executor_t *>)
        return std::make_unique<Owner> (std::move (options), std::move (context), &executor);
    else
        return std::make_unique<Owner> (std::move (options), std::move (context));
}
int main ()
try {
    fw::runtime::offload_executor_t infrastructure (1, "thread-probe-infrastructure");
    // Keep the test's receive worker in the baseline. A joined thread can
    // remain visible in /proc/self/task while the kernel finishes its exit.
    fw::runtime::offload_executor_t receive_executor (1, "thread-probe-receive");
    auto context = std::make_shared<zlink::context_t> ();
    fw::runtime::protocol::client_server_server_admission_t descriptor;
    descriptor.channel_name = "thread-probe";
    descriptor.server_routing_id = {'s'};
    descriptor.descriptor_revision = 1;
    options_t options{descriptor};
    options.runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ();
    const auto before = threads ();
    auto owner = make_owner<owner_t> (std::move (options), context, infrastructure);
    const auto after = threads ();
    std::cout << "raw source+registry expected added threads=0 actual=" << after - before << '\n';
    if (after != before) {
        std::cerr << "owner construction changed thread count: before=" << before
                  << " after=" << after << '\n';
        return 1;
    }
    // Occupying the borrowed executor must not occupy the receive owner's
    // private liveness state. This uses the same pre-existing task API in RED
    // and GREEN, without opening a socket.
    {
        std::promise<void> entered;
        std::promise<void> release;
        auto released = release.get_future ().share ();
        infrastructure.submit ([&entered, released] {
            entered.set_value ();
            released.wait ();
        });
        entered.get_future ().wait ();
        auto finished = std::make_shared<std::promise<void>> ();
        auto terminal = finished->get_future ();
        receive_executor.submit ([&, finished] {
            try {
                (void) owner
                  ->tick_liveness (fw::runtime::mesh::service_liveness_registry_t::clock_t::now ())
                  .result ()
                  .value ();
                (void) owner->next_liveness_activity_task ().result ().value ();
                finished->set_value ();
            }
            catch (...) {
                finished->set_exception (std::current_exception ());
            }
        });
        const auto status = terminal.wait_for (std::chrono::seconds (2));
        release.set_value ();
        terminal.get ();
        if (status != std::future_status::ready) {
            std::cerr << "receive owner waited for the occupied state executor\n";
            return 5;
        }
    }
    // The automatic API delegates to the same synchronous owner state body.
    if (owner->next_liveness_activity_task ().result ().value ()) {
        std::cerr << "empty owner reported a next liveness activity\n";
        return 2;
    }
    auto changed = owner->descriptor_task ().result ().value ();
    ++changed.descriptor_revision;
    changed.weight = 37;
    owner->update_descriptor_task (changed).result ().value ();
    const auto weight = owner->descriptor_task ().result ().value ().weight;
    if (weight != 37) {
        std::cerr << "descriptor update lost weight: expected=37 actual=" << weight << '\n';
        return 3;
    }
    (void) owner->tick_liveness (fw::runtime::mesh::service_liveness_registry_t::clock_t::now ())
      .result ()
      .value ();
    const auto settled_threads = threads ();
    if (settled_threads != before) {
        std::cerr << "owner operations changed thread count: expected=" << before
                  << " actual=" << settled_threads << '\n';
        return 4;
    }
    // Exercise the shared registry rule bodies through their existing serialized
    // interface as well: valid and invalid route/probe inputs must differ.
    {
        using registry_t = fw::runtime::mesh::service_liveness_registry_t;
        fw::runtime::state_lane_t registry_lane (infrastructure);
        registry_t registry (registry_lane, registry_t::default_probe_interval,
                             registry_t::default_peer_timeout);
        const std::vector<std::uint8_t> peer{'p'}, route{'r'}, wrong{'x'};
        const auto now = registry_t::clock_t::time_point{};
        auto connection = registry.admit (peer, route, now);
        if (registry.admit (peer, route, now) != connection || registry.size () != 1) {
            std::cerr << "registry admission did not retain one connection: size="
                      << registry.size () << '\n';
            return 6;
        }
        auto tick = registry.tick (now);
        if (tick.probes.size () != 1 || tick.probes.front ().node_routing_id != peer) {
            std::cerr << "registry first tick did not probe peer p: probes=" << tick.probes.size ()
                      << '\n';
            return 7;
        }
        const auto probe = tick.probes.front ().probe_id;
        if (registry.acknowledge (peer, wrong, probe, now)
            || registry.acknowledge_probe (peer, wrong, probe)
            || registry.acknowledge_probe (peer, route, 0)
            || !registry.acknowledge_probe (peer, route, probe)
            || !registry.acknowledge (peer, route, probe, now)) {
            std::cerr << "registry ACK acceptance disagreed with route/probe identity: probe="
                      << probe << '\n';
            return 8;
        }
        if (registry.next_activity () != now + registry_t::default_probe_interval
            || !registry.tick (now).probes.empty ()) {
            std::cerr << "registry ACK did not schedule only the next probe interval\n";
            return 9;
        }
        if (registry.disconnect (peer, wrong) || !registry.disconnect (peer, route)) {
            std::cerr << "registry disconnect acceptance disagreed with route identity\n";
            return 10;
        }
        (void) registry.tick (now + registry_t::default_probe_interval);
        if (registry.next_activity () || registry.size () != 0) {
            std::cerr << "registry retained activity after disconnect: size=" << registry.size ()
                      << '\n';
            return 11;
        }
        (void) registry.admit (peer, route, now);
        const auto expired = registry.tick (now + registry_t::default_peer_timeout);
        if (expired.timed_out_nodes != std::vector<std::vector<std::uint8_t>>{peer}
            || registry.size () != 0) {
            std::cerr << "registry timeout did not remove only peer p: timed_out_nodes="
                      << expired.timed_out_nodes.size () << " size=" << registry.size () << '\n';
            return 12;
        }
        std::cout << "registry admission/probe/ACK/disconnect/timeout positive and negative inputs "
                     "passed\n";
    }
    owner.reset ();
    std::cout << "nested state lane/descriptor/tick/close completed; added threads=0\n";
}
catch (const std::exception &error) {
    std::cerr << "manual source thread test threw: " << error.what () << '\n';
    throw;
}
