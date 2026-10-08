/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §11.1: independent CS process -> Session process; physical connector per global client ID.
// `request(dto).submit<PerfEchoReply>(callback)` through full identity/byte validation is one operation.
// 1024/4096 JSON, request/ordinary, immediate public dispatch mode; no Store or Actor.
// Server logical stream, Actor, Spot, worker and fanout metrics are not applicable.
//
// The client role shares this connector loop between the session baseline and CS cells; the cell id identifies the scenario.

#include <perf/measurement.hpp>

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/stream_connector.hpp>

namespace perf
{
namespace sc = zlink::stream_connector;

class session_echo_only_scenario_t
{
  public:
    session_echo_only_scenario_t (const endpoint_manifest_t &manifest,
                                  measurement_t &measurement,
                                  int index) :
        _manifest (manifest), _measurement (measurement), _index (index)
    {
        const auto &workload = manifest.workload;
        const auto total = *workload.connections;
        const auto quotient = total / workload.client_count;
        const auto remainder = total % workload.client_count;
        _count = quotient + (index < remainder ? 1 : 0);
        _first = index * quotient + std::min (index, remainder);
        _sequences = std::vector<std::atomic<std::uint64_t>> (static_cast<std::size_t> (_count));
        for (const auto &role : manifest.roles)
            if (role.stream_endpoint)
                _endpoint = *role.stream_endpoint;
    }
    ~session_echo_only_scenario_t ()
    {
        for (auto &slot : _connectors)
            (void) slot.connector->close ();
    }

    // Connect/setup is bounded by connect-concurrency per process and starts each connector exactly once; the setup
    // probe is a typed echo that also carries the session's create and bind for an Actor cell.
    void prepare ()
    {
        const auto &workload = _manifest.workload;
        json evidence = json::array ();
        evidence.get_ref<json::array_t &> ().resize (static_cast<std::size_t> (_count));
        std::atomic<int> next{0};
        std::mutex gate;
        std::vector<std::thread> workers;
        for (int w = 0; w < std::min (*workload.connect_concurrency, std::max (_count, 1)); ++w)
            workers.emplace_back ([&] {
                for (int local; (local = next.fetch_add (1)) < _count;) {
                    const auto id = _first + local;
                    const auto started = now_ticks ();
                    try {
                        sc::connector_options_t options;
                        options.endpoint = _endpoint;
                        options.dispatch_mode = sc::dispatch_mode_t::immediate;
                        options.connect_timeout =
                          std::chrono::milliseconds (workload.setup_timeout_ms);
                        options.request_timeout =
                          std::chrono::milliseconds (workload.setup_timeout_ms);
                        auto connector = std::make_shared<sc::connector_t> (
                          sc::connector_factory_t::create (options));
                        if (const auto connected = connector->connect (); !connected)
                            throw connector_error_t (
                              std::to_string (static_cast<int> (connected.error ()->code)), false,
                              connected.error ()->message);
                        const auto request = _measurement.request (
                          id, _sequences[static_cast<std::size_t> (local)].fetch_add (1) + 1, true);
                        // Setup probe: bounded by the setup deadline.
                        const auto reply =
                          connector->request (request)
                            .timeout (std::chrono::milliseconds (workload.setup_timeout_ms))
                            .submit<echo_reply_t> ();
                        if (!reply)
                            throw connector_error_t (
                              std::to_string (static_cast<int> (reply.error ()->code)),
                              reply.error ()->code == sc::error_code_t::request_timeout,
                              reply.error ()->message);
                        payload_pattern_t::validate_identity (request, reply.value ());
                        _measurement.pattern ().validate (reply.value ().payload);
                        if (!connector->is_connected ())
                            throw std::runtime_error (
                              "Connector lost its connection during setup.");
                        std::lock_guard lock (gate);
                        _connectors.push_back ({id, local, connector});
                        ++_connected;
                        evidence[static_cast<std::size_t> (local)] = {
                          {"kind", "connectorSetupAndTypedProbe"},
                          {"source", "connect + is_connected + request.submit<PerfEchoReply>"},
                          {"observedValue",
                           {{"clientId", id},
                            {"isConnected", true},
                            {"setupLatencyNs", dec (now_ticks () - started)},
                            {"correlationId", request.correlation_id}}}};
                    }
                    catch (const std::exception &error) {
                        std::lock_guard lock (gate);
                        ++_failures;
                        evidence[static_cast<std::size_t> (local)] = {
                          {"kind", "connectorSetupFailure"},
                          {"source", type_name (typeid (error))},
                          {"observedValue",
                           {{"clientId", id},
                            {"message", error.what ()},
                            {"setupLatencyNs", dec (now_ticks () - started)}}}};
                    }
                }
            });
        for (auto &worker : workers)
            worker.join ();
        _measurement.set_connection_result (_connected, _failures);
        _measurement.set_setup_evidence (std::move (evidence));
    }

    // A STREAM connector has one unresolved echo at a time (§4.3).
    void run (const loops_t &loops)
    {
        for (const auto &slot : _connectors) {
            loops->enter ();
            issue (loops, slot, std::make_shared<request_chain_t> ());
        }
    }

  private:
    struct request_chain_t
    {
        std::atomic<unsigned int> work{0};
    };

    struct connector_slot_t
    {
        int id, local;
        std::shared_ptr<sc::connector_t> connector;
    };

    // The chain work counter sends inline callback completions back through this loop instead of recursive submissions.
    void issue (const loops_t &loops,
                const connector_slot_t &slot,
                const std::shared_ptr<request_chain_t> &chain)
    {
        if (chain->work.fetch_add (1, std::memory_order_acq_rel) != 0)
            return;
        while (true) {
            if (!_measurement.can_issue ()) {
                chain->work.fetch_sub (1, std::memory_order_acq_rel);
                loops->leave ();
                return;
            }
            auto request = _measurement.request (
              slot.id, _sequences[static_cast<std::size_t> (slot.local)].fetch_add (1) + 1);
            std::int64_t started = 0;
            if (!_measurement.begin_operation (started)) {
                chain->work.fetch_sub (1, std::memory_order_acq_rel);
                loops->leave ();
                return;
            }
            request.sent_ticks = dec (started);
            echo_request_t expected;
            expected.run_id = request.run_id;
            expected.cell_id = request.cell_id;
            expected.reset_seq = request.reset_seq;
            expected.phase = request.phase;
            expected.client_id = request.client_id;
            expected.sequence = request.sequence;
            expected.correlation_id = request.correlation_id;
            slot.connector->request (request)
              .timeout (_measurement.call_timeout ())
              .submit<echo_reply_t> ([this, loops, slot, expected = std::move (expected), started,
                                      chain] (sc::result_t<echo_reply_t> reply) {
                  std::exception_ptr error;
                  try {
                      if (!reply)
                          throw connector_error_t (
                            std::to_string (static_cast<int> (reply.error ()->code)),
                            reply.error ()->code == sc::error_code_t::request_timeout,
                            reply.error ()->message);
                      payload_pattern_t::validate_identity (expected, reply.value ());
                      _measurement.pattern ().validate (reply.value ().payload);
                  }
                  catch (...) {
                      error = std::current_exception ();
                  }
                  _measurement.complete_operation (started, error);
                  issue (loops, slot, chain);
              });

            if (chain->work.fetch_sub (1, std::memory_order_acq_rel) == 1)
                return;
        }
    }

    const endpoint_manifest_t &_manifest;
    measurement_t &_measurement;
    int _index, _count = 0, _first = 0;
    std::string _endpoint;
    std::vector<std::atomic<std::uint64_t>> _sequences;
    std::vector<connector_slot_t> _connectors;
    std::uint64_t _connected = 0, _failures = 0;
};
} // namespace perf
