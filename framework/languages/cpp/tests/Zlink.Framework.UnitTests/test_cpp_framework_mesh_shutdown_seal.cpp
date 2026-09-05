/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "runtime/mesh/raw_mesh_node_owner.hpp"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

namespace mesh = zlink::framework::runtime::mesh;
using namespace std::chrono_literals;

namespace
{
mesh::raw_mesh_node_options_t options (
  char rid, std::string endpoint = "tcp://127.0.0.1:0",
  std::shared_ptr<const std::atomic_bool> seal = {})
{
    mesh::raw_mesh_node_options_t result;
    result.descriptor = {"shutdown-seal", {static_cast<std::uint8_t> (rid)}, 1, 1,
                         std::move (endpoint), {{"alpha", 100}}};
    result.shutdown_admission_seal = std::move (seal);
    return result;
}

void pump (mesh::raw_mesh_node_owner_t &node)
{
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    node.drain_monitor_events (now).result ().value ();
    assert (node.pump_one (now).result ().value () != mesh::raw_mesh_pump_result_t::protocol_error);
}

template <typename Predicate, typename Progress>
void until (Predicate predicate, Progress progress)
{
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!predicate () && std::chrono::steady_clock::now () < deadline) {
        progress ();
        std::this_thread::sleep_for (1ms);
    }
    assert (predicate ());
}

void connect (mesh::raw_mesh_node_owner_t &source, mesh::raw_mesh_node_owner_t &target)
{
    const auto remote = target.topology ().local_descriptor ();
    assert (source.connect_peer (target.endpoint (), remote));
    until ([&] { return source.topology ().peers ().size () == 1
                         && target.topology ().peers ().size () == 1; },
           [&] { pump (source); pump (target); });
    // Consume both crossed Admit responses before testing a later transition.
    for (int i = 0; i < 20; ++i) {
        pump (source);
        pump (target);
        std::this_thread::sleep_for (1ms);
    }
}

void verify_restart (bool sealed, bool draining)
{
    auto seal = std::make_shared<std::atomic_bool> (false);
    mesh::raw_mesh_node_owner_t source (options ('a', "tcp://127.0.0.1:0", seal));
    auto target = std::make_unique<mesh::raw_mesh_node_owner_t> (options ('b'));
    source.start ();
    target->start ();
    connect (source, *target);
    const auto endpoint = target->endpoint ();
    seal->store (sealed, std::memory_order_release);
    if (draining) {
        source.publish_draining ().result ().value ();
        until ([&] { return target->topology ().peer ({'a'})->descriptor.state
                              == mesh::service_node_state_t::draining; },
               [&] { pump (*target); });
    }
    target->close ();
    target.reset ();
    until ([&] { return source.topology ().peers ().empty (); }, [&] { pump (source); });
    const auto expected_state = draining ? mesh::service_node_state_t::draining
                                         : mesh::service_node_state_t::serving;
    assert (source.topology ().local_descriptor ().state == expected_state);

    // A silent restarted peer proves absence of Hello on the wire, without
    // an inbound Hello from that peer initiating a separate admission.
    target = std::make_unique<mesh::raw_mesh_node_owner_t> (
      options ('b', endpoint, std::make_shared<std::atomic_bool> (true)));
    target->start ();
    std::size_t ready_events = 0;
    until ([&] { return ready_events != 0; }, [&] {
        ready_events += source.drain_monitor_events (
          mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
    });
    std::size_t remote_ready = 0;
    until ([&] { return remote_ready != 0; }, [&] {
        remote_ready += target->drain_monitor_events (
          mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
    });
    if (sealed) {
        const auto deadline = std::chrono::steady_clock::now () + 200ms;
        while (std::chrono::steady_clock::now () < deadline) {
            pump (source);
            assert (target->pump_one (mesh::service_liveness_registry_t::clock_t::now ())
                      .result ().value () == mesh::raw_mesh_pump_result_t::no_data);
            assert (source.topology ().peers ().empty ());
            assert (source.topology ().local_descriptor ().state == expected_state);
            std::this_thread::sleep_for (1ms);
        }
    } else {
        until ([&] { return source.topology ().peers ().size () == 1
                             && target->topology ().peers ().size () == 1; },
               [&] { pump (source); pump (*target); });
        assert (target->topology ().peer ({'a'})->descriptor.state == expected_state);
        assert (source.topology ().local_descriptor ().state == expected_state);
    }
    const auto close_started = std::chrono::steady_clock::now ();
    source.close ();
    target->close ();
    assert (std::chrono::steady_clock::now () - close_started < 2s);
    assert (!source.started ());
}

void verify_liveness_and_failed_update_preserve_draining ()
{
    auto seal = std::make_shared<std::atomic_bool> (false);
    mesh::raw_mesh_node_owner_t source (options ('c', "tcp://127.0.0.1:0", seal));
    mesh::raw_mesh_node_owner_t target (options ('d'));
    source.start ();
    target.start ();
    connect (source, target);
    seal->store (true, std::memory_order_release);
    source.publish_draining ().result ().value ();
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    const auto tick = source.tick_liveness (now + 16s).result ().value ();
    assert (tick.timed_out_nodes.size () == 1);
    assert (source.topology ().peers ().empty ());
    assert (source.topology ().local_descriptor ().state == mesh::service_node_state_t::draining);
    target.close ();
    // Keep an admitted logical peer after transport removal so Update meets
    // a real control-send failure; local publication must remain Draining.
    const auto peer = target.topology ().local_descriptor ();
    assert (source.admit_peer (peer, {'x'}, now) == mesh::peer_admission_result_t::admitted);
    source.disconnect_peer (target.endpoint ());
    assert (source.admit_peer (peer, {'y'}, now) == mesh::peer_admission_result_t::admitted);
    source.publish_draining ().result ().value ();
    assert (source.topology ().local_descriptor ().state == mesh::service_node_state_t::draining);
    source.close ();
}

void verify_crossed_admission_diagnostics ()
{
    mesh::raw_mesh_node_owner_t source (options ('e'));
    mesh::raw_mesh_node_owner_t target (options ('f'));
    source.start ();
    target.start ();
    std::size_t source_changes = 0;
    std::size_t target_changes = 0;
    source.topology ().set_change_handler ([&] { ++source_changes; });
    target.topology ().set_change_handler ([&] { ++target_changes; });
    std::ostringstream trace;
    auto *previous = std::cerr.rdbuf (trace.rdbuf ());
    setenv ("ZLINK_CPP_MESH_TRACE", "1", 1);
    connect (source, target);
    unsetenv ("ZLINK_CPP_MESH_TRACE");
    std::cerr.rdbuf (previous);
    source.topology ().set_change_handler ({});
    target.topology ().set_change_handler ({});
    assert (source_changes == 1);
    assert (target_changes == 1);
    const auto log = trace.str ();
    for (const auto *rid : {"65", "66"}) {
        const auto phase = log.find ("handshake phase=bilateral-ready", log.find (
          "handshake phase=local-admission-awaiting-remote-admit"));
        assert (phase != std::string::npos);
        std::istringstream lines (log);
        std::string line;
        bool found = false;
        while (std::getline (lines, line)) {
            if (line.find ("handshake phase=bilateral-ready") != std::string::npos
                && line.find (std::string ("source=") + rid + " result=1 peer=present")
                     != std::string::npos)
                found = true;
        }
        assert (found);
    }
    const auto before = source.topology ().peer ({'f'});
    assert (source.topology ().admit (before->descriptor, before->connection_id,
                                     before->direction)
            == mesh::peer_admission_result_t::duplicate_connection);
    assert (source.topology ().peer ({'f'})->admission_epoch == before->admission_epoch);
}
} // namespace

int main ()
{
    verify_crossed_admission_diagnostics ();
    verify_restart (true, true);
    verify_restart (false, false);
    verify_restart (false, true);
    verify_liveness_and_failed_update_preserve_draining ();
}
