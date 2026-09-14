/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// Test-only incremental cost experiment. Every enabled feature calls its
// owning runtime module; compile-time selection never changes the libraries.
#include "bench_async.hpp"
#include "bench_common.hpp"
#include "bench_stats_server.hpp"
#include "perf_monitor_wait.hpp"
#include <zlink/codecs/protobuf.hpp>
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/mesh/service_mailbox.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <csignal>
#include <condition_variable>
#include <iostream>

namespace bench = zlink_cpp_bench;
namespace fw = zlink::framework;
namespace messaging = fw::runtime::messaging;
namespace wire = fw::runtime::protocol;
namespace mesh = fw::runtime::mesh;
using payload_t = zlink::framework::bench::withgrpc::BenchPayload;
using parts_t = std::vector<zlink::message_t>;
using clock_type = std::chrono::steady_clock;
constexpr int stage = BENCH_DIAGNOSTIC_STAGE;
std::atomic<bool> stopped {false};
void stop_signal (int) { stopped = true; }

void require (bool value, const char *reason)
{
    if (!value) throw std::runtime_error (reason);
}

class features_t
{
  public:
    features_t ()
    {
        // Register the same extension as production RouteMesh configuration.
        // Registry lookup remains part of the codec stage.
        fw::codec_options_builder_t (serializers).use (zlink::framework_codecs::protobuf ());
    }

    parts_t encode (const payload_t &payload, bool response, bool command,
                    std::uint64_t correlation)
    {
        parts_t parts;
        zlink::message_t body;
        if constexpr (stage == 0) {
            // Core-like removes only Framework registry dispatch. It still
            // serializes the real BenchPayload protobuf on every message.
            const auto size = payload.ByteSizeLong ();
            require (size <= static_cast<std::size_t> (std::numeric_limits<int>::max ()),
                     "protobuf payload exceeds serializer size limit");
            body = zlink::message_t::allocate (size);
            require (body.valid () && payload.SerializeToArray (body.data (), static_cast<int> (size)),
                     "core protobuf serialization failed");
        } else {
            auto serializer = serializers.get<payload_t> ();
            body = fw::detail::encoded_payload_to_raw (serializer.serialize (payload));
        }
        if constexpr (stage >= 2) {
            messaging::envelope_header_t header;
            header.kind = response ? messaging::message_kind_t::response
              : command ? messaging::message_kind_t::command : messaging::message_kind_t::request;
            header.channel_name = "bench";
            header.message_name = "BenchPayload";
            header.content_type = "application/x-protobuf";
            header.correlation_id = std::to_string (correlation);
            parts = envelope.encode_raw_body_parts (header, std::move (body)).take_items ();
        } else {
            // A fixed transport header keeps the core and codec frame counts
            // identical. It is not an alternative Framework envelope codec.
            const char *header = response ? bench::response_envelope () : bench::request_envelope ();
            parts.push_back (zlink::message_t::from (std::as_bytes (
              std::span<const char> (header, std::strlen (header)))));
            parts.push_back (std::move (body));
        }
        if constexpr (stage >= 3) {
            auto packed = wire::encode_application_payload (
              wire::application_payload_t::from_parts (std::move (parts)));
            auto header = response ? wire::encode_reply_header (correlation, 0, 0)
              : command ? wire::encode_node_send_header () : wire::encode_node_request_header (correlation);
            parts.clear ();
            parts.push_back (zlink::message_t::from (std::span<const std::uint8_t> (header)));
            parts.push_back (zlink::message_t::from (std::span<const std::uint8_t> (packed)));
        }
        return parts;
    }

    payload_t decode (parts_t parts, bool response, bool command,
                      std::uint64_t *correlation = nullptr)
    {
        require (parts.size () == 2, "expected two transport frames");
        if constexpr (stage >= 3) {
            const auto bytes = parts[0].bytes ();
            const std::span<const std::uint8_t> header (
              reinterpret_cast<const std::uint8_t *> (bytes.data ()), bytes.size ());
            if (response) {
                auto reply = wire::decode_reply_header (header);
                if (correlation) *correlation = reply.correlation;
            } else if (!command) {
                auto id = wire::decode_node_request_header (header);
                if (correlation) *correlation = id;
            } else {
                require (wire::decode_header (header).kind == wire::command::nodeSend,
                         "expected nodeSend command");
            }
            const auto body = parts[1].bytes ();
            parts = wire::decode_application_parts (wire::decode_application_payload (
              {reinterpret_cast<const std::uint8_t *> (body.data ()), body.size ()}, false));
        }
        if constexpr (stage >= 2) {
            auto decoded = envelope.decode_header (parts[0], false);
            require (decoded.has_value (), "invalid Framework envelope");
            const auto &header = decoded.value ();
            require (header.message_name == "BenchPayload"
                       && header.content_type == "application/x-protobuf", "invalid typed envelope");
            require (header.kind == (response ? messaging::message_kind_t::response
                       : command ? messaging::message_kind_t::command : messaging::message_kind_t::request),
                     "incorrect envelope message kind");
            if (correlation) *correlation = std::stoull (header.correlation_id);
        }
        payload_t payload;
        if constexpr (stage == 0) {
            require (parts[1].size () <= static_cast<std::size_t> (std::numeric_limits<int>::max ())
                       && payload.ParseFromArray (parts[1].data (), static_cast<int> (parts[1].size ())),
                     "core protobuf parsing failed");
        } else {
            payload = serializers.get<payload_t> ().deserialize (
              fw::detail::encoded_payload_from_raw (std::move (parts[1])));
        }
        return payload;
    }

    // Inline mailbox isolation includes the real retention representation and
    // claim/release state machine; it does not claim to measure worker dispatch.
    parts_t admit (const parts_t &parts)
    {
        if constexpr (stage < 4) return parts;
        mesh::service_mailbox_record_t record;
        record.owner = mesh::service_mailbox_t::application_owner (fw::runtime::host::owner_kind_t::node);
        record.domain = mesh::service_mailbox_domain_t::application;
        for (const auto &part : parts) {
            const auto bytes = part.bytes ();
            const auto *data = reinterpret_cast<const std::uint8_t *> (bytes.data ());
            record.parts.emplace_back (data, data + bytes.size ());
        }
        require (mailbox.try_enqueue (std::move (record)), "mailbox closed");
        auto claim = mailbox.try_claim (mesh::service_mailbox_domain_t::application,
                                       1, std::numeric_limits<std::size_t>::max ());
        require (claim && claim->records.size () == 1, "mailbox claim failed");
        parts_t owned;
        for (const auto &part : claim->records[0].parts)
            owned.push_back (zlink::message_t::from (std::span<const std::uint8_t> (part)));
        require (mailbox.release (*claim), "mailbox release failed");
        return owned;
    }

  private:
    fw::serializer_registry_t serializers;
    messaging::envelope_codec_t envelope;
    mesh::service_mailbox_t mailbox;
};

payload_t make_payload (std::size_t size, bench::phase_t phase, std::uint64_t sequence)
{
    payload_t payload;
    payload.mutable_body ()->assign (size, '\xab');
    require (bench::stamp_payload (payload.mutable_body ()->data (), size, 1, phase, sequence),
             "cannot stamp diagnostic payload");
    return payload;
}

payload_t make_response (const payload_t &request)
{
    bench::decoded_header_t stamp;
    require (bench::decode_payload (request.body ().data (), request.body ().size (), &stamp),
             "request stamp invalid");
    payload_t response;
    response.mutable_body ()->assign (4096, '\xab');
    std::copy_n (request.body ().data (), bench::k_header_size, response.mutable_body ()->data ());
    bench::write_u32_le (reinterpret_cast<unsigned char *> (response.mutable_body ()->data ()) + 9, 4096);
    return response;
}

void fidelity ()
{
    features_t features;
    for (bool command : {false, true}) {
        auto original = make_payload (command ? 4096 : 64, bench::phase_active, 42);
        if constexpr (stage <= 1) {
            const auto parts = features.encode (original, false, command, 42);
            const auto expected = bench::encode_bench_payload (original);
            require (parts[1].size () == expected.size ()
                       && std::memcmp (parts[1].data (), expected.data (), expected.size ()) == 0,
                     "core/codec protobuf wire fidelity failed");
            payload_t parsed;
            require (parsed.ParseFromArray (parts[1].data (), static_cast<int> (parts[1].size ()))
                       && parsed.body () == original.body (), "protobuf decoded content differs");
        }
        std::uint64_t correlation = 42;
        auto decoded = features.decode (features.admit (features.encode (original, false, command, 42)),
                                        false, command, &correlation);
        require (decoded.body () == original.body () && correlation == 42, "input fidelity failed");
        if (!command) {
            const auto expected_response = make_response (decoded);
            auto response_parts = features.encode (expected_response, true, false, 42);
            if constexpr (stage <= 1) {
                const auto expected = bench::encode_bench_payload (expected_response);
                require (response_parts[1].size () == expected.size ()
                           && std::memcmp (response_parts[1].data (), expected.data (), expected.size ()) == 0,
                         "response protobuf wire fidelity failed");
            }
            auto response = features.decode (std::move (response_parts), true, false, &correlation);
            require (response.body () == expected_response.body (), "response content differs");
            bench::decoded_header_t before, after;
            require (bench::decode_payload (original.body ().data (), original.body ().size (), &before)
                       && bench::decode_payload (response.body ().data (), response.body ().size (), &after),
                     "response stamp invalid");
            require (response.body ().size () == 4096 && after.payload_size == 4096
                       && after.run_id == before.run_id && after.phase == before.phase
                       && after.seq == before.seq && after.sent_ns == before.sent_ns,
                     "response fidelity failed");
        }
    }
    std::cout << "Fidelity passed stage=" << BENCH_DIAGNOSTIC_STAGE_NAME << '\n';
}

void target (const std::string &endpoint, bool command, const std::string &output, int stats_port)
{
    zlink::context_t context;
    zlink::router_socket_t socket (context);
    socket.set_routing_id (zlink::routing_id_t::from ("cpp-diagnostic-target"));
    socket.bind (endpoint);
    features_t features;
    bench::server_metrics_t metrics;
    bench::stats_http_server_t control (std::vector<int>{stats_port},
      [&] (const std::string &method, const std::string &path, const std::string &body) -> bench::bench_http_reply_t {
          if (method == "GET" && path == "/bench/stats") return {200, metrics.snapshot_json ()};
          if (method == "POST" && path == "/bench/start"
              && nlohmann::json::parse (body).value ("phase", "") == "close") {
              stopped = true;
              context.shutdown ();
              return {200, "{\"ok\":true}"};
          }
          return {404, "{}"};
      });
    require (control.start (), "target control host failed");
    std::cout << "READY" << std::endl;
    zlink::received_t received;
    while (!stopped) {
        const int rc = socket.recv (received, zlink::recv_flags_t::none);
        if (rc != 0) {
            // The pinned ROUTER int receive preserves the legacy raw return;
            // capture its errno through the public error object immediately.
            const zlink::recv_error_t error (static_cast<zlink::recv_result_t> (rc));
            if (stopped && error.internal_errno () == ETERM) break;
            throw error;
        }
        std::uint64_t correlation = 0;
        auto payload = features.decode (features.admit (received.parts ()), false, command, &correlation);
        bench::decoded_header_t stamp;
        require (bench::decode_payload (payload.body ().data (), payload.body ().size (), &stamp)
                   && payload.body ().size () == (command ? 4096 : 64), "target payload invalid");
        metrics.record (payload.body ().data (), payload.body ().size ());
        if (!command) {
            auto reply = features.encode (make_response (payload), true, false, stamp.seq);
            received.reply ().message (reply[0]).message (reply[1]).submit ();
        }
    }
    if (!output.empty ()) {
        std::ofstream file (output);
        file << metrics.snapshot_json () << '\n';
    }
}

// The source's native socket remains alive while the shared runner settle
// owner reads immutable terminal source counts and live target metrics.
void await_runner_close (const nlohmann::json &result, int stats_port)
{
    std::mutex mutex;
    std::condition_variable closed;
    bool close_requested = false;
    bench::stats_http_server_t control (std::vector<int>{stats_port},
      [&] (const std::string &method, const std::string &path, const std::string &body) -> bench::bench_http_reply_t {
          if (method == "GET" && path == "/bench/stats") return {200, result.dump ()};
          if (method == "POST" && path == "/bench/start"
              && nlohmann::json::parse (body).value ("phase", "") == "close") {
              { std::lock_guard lock (mutex); close_requested = true; }
              closed.notify_one ();
              return {200, "{\"ok\":true}"};
          }
          return {404, "{}"};
      });
    require (control.start (), "source control host failed");
    std::cout << "IDLE" << std::endl;
    std::unique_lock lock (mutex);
    closed.wait (lock, [&] { return close_requested; });
}

class source_t
{
  public:
    source_t (const std::string &endpoint, bool command) : socket (context), command (command)
    {
        socket.set_routing_id (zlink::routing_id_t::from ("cpp-diagnostic-source"));
        auto monitor = socket.monitor_open (zlink::monitor_event::connection_ready);
        socket.connect (endpoint);
        // Raw perf readiness belongs to Core's logical CONNECTION_READY edge.
        // Preparation is outside every measured window and never sends probes.
        require (perf::wait_socket_monitor_event (
          monitor, static_cast<std::uint64_t> (zlink::monitor_event::connection_ready), 10000),
          "Core connection readiness preparation failed");
        monitor.close ();
        poller.add (socket, zlink::poll_event_flag_t::pollcompletion, 1);
    }

    nlohmann::json run (const std::string &pattern, double seconds, bench::phase_t phase, bool once = false)
    {
        deadline = clock_type::now () + std::chrono::duration_cast<clock_type::duration> (
          std::chrono::duration<double> (seconds));
        submitted = completed = 0;
        latency_sum = 0;
        const auto started = bench::now_ns ();
        const auto cpu_started = bench::process_cpu_seconds_self ();
        std::vector<bench::task_t> tasks;
        const int slots = command ? 8 : pattern == "request-backpressure" ? 100 : 1;
        for (int i = 0; i < slots; ++i) tasks.push_back (slot (phase, once));
        const auto drain_deadline = deadline + std::chrono::seconds (30);
        std::vector<zlink::poll_event_t> events (1);
        for (;;) {
            const auto resumed = ready.run_ready_round ();
            if (std::all_of (tasks.begin (), tasks.end (), [] (const auto &task) { return task.done (); })) break;
            const auto now = clock_type::now ();
            require (now < drain_deadline, "diagnostic drain bound exceeded");
            const auto wait = resumed != 0 ? std::chrono::milliseconds (0)
              : bench::poll_timeout_until (now, now < deadline ? deadline : drain_deadline,
                                          bench::poll_max_wait_ms);
            poller.wait (events.data (), events.size (), wait);
        }
        for (const auto &task : tasks) if (task.failure ()) std::rethrow_exception (task.failure ());
        const auto elapsed = (bench::now_ns () - started) / 1e9;
        return {{"diagnosticOnly", true}, {"stage", BENCH_DIAGNOSTIC_STAGE_NAME},
          {"pattern", pattern}, {"requestBytes", 64}, {"responseBytes", 4096}, {"sendBytes", 4096},
          {"durationSeconds", seconds}, {"elapsedSeconds", elapsed}, {"submitted", submitted},
          {"completed", completed}, {"errors", 0}, {"drainBoundHit", false},
          {"sourceCompletedPerSecond", completed / seconds},
          {"sourceCpuPercent", (bench::process_cpu_seconds_self () - cpu_started)
            / elapsed / bench::logical_cores () * 100},
          {"sourceMemoryMb", bench::rss_mb ()},
          {"meanUs", completed ? latency_sum / completed / 1000.0 : 0},
          {"mailboxMode", "inline-claim-release"}, {"featureSelection", "compile-time"}};
    }

  private:
    bench::task_t slot (bench::phase_t phase, bool once)
    {
        co_await ready.schedule ();
        do {
            if (!once && clock_type::now () >= deadline) break;
            const auto seq = ++sequence;
            auto payload = make_payload (command ? 4096 : 64, phase, seq);
            bench::decoded_header_t sent_stamp;
            bench::decode_payload (payload.body ().data (), payload.body ().size (), &sent_stamp);
            auto parts = features.encode (payload, false, command, seq);
            ++submitted;
            if (command) {
                auto submission = socket.send (zlink::routing_id_t::from ("cpp-diagnostic-target"))
                  .message (parts[0]).message (parts[1]).async ();
                if (submission.result == ZLINK_SUBMIT_BACKPRESSURED) co_await std::move (submission.admitted);
                else require (submission.result == ZLINK_SUBMIT_OK, "send submission failed");
            } else {
                auto submission = socket.request (zlink::routing_id_t::from ("cpp-diagnostic-target"))
                  .message (parts[0]).message (parts[1]).timeout (std::chrono::seconds (30)).async ();
                if (submission.result == ZLINK_SUBMIT_BACKPRESSURED) co_await std::move (submission.admitted);
                else require (submission.result == ZLINK_SUBMIT_OK, "request submission failed");
                auto reply = features.decode (co_await std::move (submission.reply), true, false);
                bench::decoded_header_t stamp;
                require (bench::decode_payload (reply.body ().data (), reply.body ().size (), &stamp)
                           && reply.body ().size () == 4096 && stamp.payload_size == 4096
                           && stamp.run_id == 1 && stamp.phase == phase && stamp.seq == seq
                           && stamp.sent_ns == sent_stamp.sent_ns, "source response invalid");
                latency_sum += bench::now_ns () - sent_stamp.sent_ns;
            }
            ++completed;
            co_await ready.schedule ();
        } while (!once);
    }

    zlink::context_t context;
    zlink::router_socket_t socket;
    zlink::poller_t poller;
    bench::ready_queue_t ready;
    features_t features;
    bool command;
    clock_type::time_point deadline;
    std::uint64_t sequence = 0, submitted = 0, completed = 0;
    double latency_sum = 0;
};

int main (int argc, char **argv)
{
    try {
        const auto role = bench::arg_value (argc, argv, "--role", "info");
        const auto pattern = bench::arg_value (argc, argv, "--pattern", "request-serial");
        require (pattern == "request-serial" || pattern == "request-backpressure" || pattern == "send-saturation",
                 "unsupported diagnostic pattern");
        const bool command = pattern == "send-saturation";
        const auto endpoint = bench::arg_value (argc, argv, "--endpoint", "tcp://127.0.0.1:5298");
        const auto output = bench::arg_value (argc, argv, "--output", "");
        if (role == "info") {
            std::cout << nlohmann::json {{"stage", BENCH_DIAGNOSTIC_STAGE_NAME},
              {"featureSelection", "compile-time"}, {"requestBytes", 64}, {"responseBytes", 4096},
              {"sendBytes", 4096}, {"mailboxMode", "inline-claim-release"}}.dump () << '\n';
        } else if (role == "fidelity") fidelity ();
        else if (role == "target") {
            std::signal (SIGTERM, stop_signal);
            std::signal (SIGINT, stop_signal);
            target (endpoint, command, output, std::stoi (bench::arg_value (argc, argv, "--stats-port", "5299")));
        } else if (role == "source" || role == "selftest") {
            source_t source (endpoint, command);
            {
                const double warmup = std::stod (bench::arg_value (argc, argv, "--warmup", "2"));
                const double duration = std::stod (bench::arg_value (argc, argv, "--duration", "5"));
                require (warmup > 0 && duration > 0, "duration and warmup must be positive");
                auto warmup_result = source.run (pattern, warmup, bench::phase_warmup, role == "selftest");
                const int target_pid = std::stoi (bench::arg_value (argc, argv, "--target-pid", "0"));
                require (target_pid > 0, "measurement requires target process id for resource snapshots");
                const auto target_cpu_started = bench::process_cpu_seconds_pid (target_pid);
                auto result = source.run (pattern, duration, bench::phase_active, role == "selftest");
                result["warmupSeconds"] = warmup;
                result["warmupSubmitted"] = warmup_result.at ("submitted");
                result["warmupCompleted"] = warmup_result.at ("completed");
                result["currentInFlight"] = 0;
                result["phase"] = "idle";
                result["ready"] = true;
                const int target_stats_port = std::stoi (bench::arg_value (argc, argv, "--target-stats-port", "5299"));
                const auto boundary = bench::fetch_stats ("127.0.0.1", target_stats_port);
                require (boundary.has_value (), "target boundary metrics unavailable");
                result["serverReceivedAtClose"] = boundary->active_messages;
                result["targetCpuPercent"] = (bench::process_cpu_seconds_pid (target_pid) - target_cpu_started)
                  / result.at ("elapsedSeconds").get<double> () / bench::logical_cores () * 100;
                result["targetMemoryMb"] = bench::rss_mb_pid (target_pid);
                if (output.empty ()) std::cout << result.dump (2) << '\n';
                else { std::ofstream file (output); file << result.dump (2) << '\n'; }
                await_runner_close (result, std::stoi (bench::arg_value (argc, argv, "--stats-port", "5297")));
            }
        } else throw std::runtime_error ("unsupported diagnostic role");
        return 0;
    } catch (const zlink::binding_error_t &error) {
        std::cerr << nlohmann::json {{"ErrorResult", error.code ()},
          {"internalErrno", error.internal_errno ()}, {"message", error.what ()}}.dump () << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "diagnostic failed: " << error.what () << '\n';
        return 1;
    }
}
