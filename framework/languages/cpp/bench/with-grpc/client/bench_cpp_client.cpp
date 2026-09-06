/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// C++ with-grpc bench client.
//
// One client process drives the whole 18-cell grid (3 implementations x 3
// patterns x 2 payload sizes) against three server processes, and emits cell
// records only. Tables, medians, G5 and the spec 7.2 ratios belong to
// framework/bench/tools (plan 4.1, FB-020): nothing in this file judges anything.
//
// What the four earlier languages made mandatory, all present here:
//   FB-013  send throughput is sampled at the ACTIVE WINDOW BOUNDARY
//   FB-008  settle is a drain confirmation with a bound; exceeding the bound
//           marks the next cell on the same server contaminated
//   FB-017  peak_in_flight and abandoned per cell
//   G8      Little's-law depth is derivable because both throughput and mean
//           latency are reported per cell
//   ---     cell isolation: one failed cell never takes the rest of the run
//   ---     bounded route readiness before warmup, never inside it
//   FB-021  structured `with-grpc-cell-v1` output
//
// SUBMIT MODEL. Every driver here runs its submit loop and its completion drain
// on ONE application thread, and concurrency is expressed as outstanding
// operations rather than as threads. This is deliberate and it is what makes
// formula 1 meaningful for C++: `zlink-c`, the denominator, is a single-threaded
// submit loop (bindings/c/bench/with_grpc/zlink/bench_zlink_client.cpp), so a
// multi-threaded C++ numerator would divide two different experiments. The
// declared submit parallelism is therefore 1 for every cell.
#include "../common/bench_async.hpp"
#include "../common/bench_common.hpp"

#include "bench.grpc.pb.h"
#include "bench.pb.h"

#include <grpcpp/grpcpp.h>

#include <zlink.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <sys/stat.h>
#include <sys/syscall.h>

using namespace zlink_cpp_bench;
using clock_t_ = std::chrono::steady_clock;

namespace
{

// ---------------------------------------------------------------------------
// per-thread CPU, for the saturation instrument
//
// spec 5.1 / FB-023 / FB-032: the instrument must measure the execution resource
// USER CODE runs on. Process CPU is not that in C++ for exactly the reason it
// was not that in node or java: Core runs native I/O threads in this process,
// and they execute no bench code. Dividing a `zlink-cpp` row by a `grpc-cpp` row
// on process cores would compare unlike quantities. Both readings are recorded
// so the choice is visible in the data rather than asserted.
// ---------------------------------------------------------------------------

double thread_cpu_seconds_tid (pid_t tid)
{
    std::ifstream stat ("/proc/self/task/" + std::to_string (tid) + "/stat");
    std::string line;
    if (!std::getline (stat, line))
        return 0.0;
    const size_t end_comm = line.rfind (')');
    if (end_comm == std::string::npos || end_comm + 2 >= line.size ())
        return 0.0;
    std::istringstream fields (line.substr (end_comm + 2));
    std::string token;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    for (int field = 3; fields >> token; ++field) {
        if (field == 14)
            utime = std::strtoull (token.c_str (), nullptr, 10);
        else if (field == 15) {
            stime = std::strtoull (token.c_str (), nullptr, 10);
            break;
        }
    }
    const long ticks = std::max<long> (1, sysconf (_SC_CLK_TCK));
    return static_cast<double> (utime + stime) / static_cast<double> (ticks);
}

pid_t current_tid ()
{
    return static_cast<pid_t> (::syscall (SYS_gettid));
}

// ---------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------

struct options_t
{
    std::string host = "127.0.0.1";
    std::string grpc_endpoint = "127.0.0.1:5111";
    int grpc_stats_port = 5114;
    std::string framework_endpoint = "tcp://127.0.0.1:5112";
    int framework_stats_port = 5113;
    std::string raw_request_endpoint = "tcp://127.0.0.1:5115";
    int raw_stats_port = 5116;
    std::string raw_command_endpoint = "tcp://127.0.0.1:5117";

    std::string raw_request_rid = "zlink-cpp-bench-request-server";
    std::string raw_command_rid = "zlink-cpp-bench-command-server";
    std::string raw_socket = "router"; // FB-001 / spec 1.3; "dealer" for the legacy comparison

    std::vector<size_t> payload_sizes {1024, 4096};
    std::vector<std::string> implementations {"grpc-cpp", "zlink-cpp", "zlink-framework-cpp"};
    std::vector<std::string> patterns {"request-serial", "request-window", "send-saturation"};

    double duration_seconds = 5.0;
    double warmup_seconds = 5.0;
    int warmup_segments = 10;
    int request_window = 100;
    int send_concurrency = 8;
    int request_timeout_ms = 30000;
    int drain_bound_ms = 30000; // spec 3 baseline
    int readiness_timeout_ms = 20000;
    size_t latency_sample_limit = 200000;

    std::string output_dir = "log/adhoc";
    std::string run_label = "cpp-router-1";
};

// ---------------------------------------------------------------------------
// per-cell measurement state
// ---------------------------------------------------------------------------

struct counters_t
{
    std::atomic<long long> completed {0};
    std::atomic<long long> errors {0};
    std::atomic<long long> submitted {0};
    std::atomic<long long> header_failures {0};
    std::atomic<long long> outstanding {0};
    std::atomic<long long> peak_in_flight {0};

    void enter ()
    {
        const long long now = outstanding.fetch_add (1, std::memory_order_acq_rel) + 1;
        long long peak = peak_in_flight.load (std::memory_order_relaxed);
        while (now > peak
               && !peak_in_flight.compare_exchange_weak (peak, now, std::memory_order_relaxed))
            ;
    }

    void leave () { outstanding.fetch_sub (1, std::memory_order_acq_rel); }

    void reset ()
    {
        completed.store (0);
        errors.store (0);
        submitted.store (0);
        header_failures.store (0);
        peak_in_flight.store (0);
        // `outstanding` is deliberately NOT reset: requests issued during warmup
        // that are still open are genuinely still open when the active window
        // starts, and zeroing the counter here would hide them.
    }
};

// The phase a driver is in. Drivers stamp it into the payload header (spec 6) so
// the servers can separate warmup traffic from measured traffic.
struct phase_state_t
{
    std::atomic<int> phase {phase_warmup};
    std::atomic<bool> stop {false};
};

// ---------------------------------------------------------------------------
// server stats helpers
// ---------------------------------------------------------------------------

struct server_endpoint_t
{
    std::string host;
    int port = 0;
    std::string name;
};

struct drain_outcome_t
{
    double drain_ms = 0.0;
    bool bound_hit = false;
    long long received_post_drain = 0;
};

// FB-008 / spec 3: no fixed sleep. Poll what the server has received until it
// stops moving, bounded. Exceeding the bound is recorded and contaminates the
// next cell on the same server rather than being smoothed over.
drain_outcome_t drain_confirmed (const server_endpoint_t &server, int bound_ms)
{
    drain_outcome_t outcome;
    const auto start = clock_t_::now ();
    const auto deadline = start + std::chrono::milliseconds (bound_ms);
    long long last = -1;
    int stable_rounds = 0;
    for (;;) {
        const auto stats = fetch_stats (server.host, server.port);
        const long long current = stats ? stats->any_phase_messages : last;
        if (stats) {
            if (current == last) {
                if (++stable_rounds >= 3)
                    break;
            } else {
                stable_rounds = 0;
                last = current;
            }
            outcome.received_post_drain = stats->active_messages;
        }
        if (clock_t_::now () >= deadline) {
            outcome.bound_hit = true;
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    outcome.drain_ms =
      std::chrono::duration<double, std::milli> (clock_t_::now () - start).count ();
    return outcome;
}

// ---------------------------------------------------------------------------
// driver contract
//
// A driver owns one (implementation, pattern) pair and is asked to run a phase
// of a given length at a given payload size. The cell runner around it is shared
// so warmup, readiness, boundary sampling and drain are identical for the three
// implementations and cannot drift between rows.
// ---------------------------------------------------------------------------

struct phase_result_t
{
    double elapsed_s = 0.0;
    long long completed = 0;
};

class driver_t
{
  public:
    virtual ~driver_t () = default;
    // Bounded readiness: one successful round trip (or one accepted send) before
    // warmup. Never called inside a measured window.
    virtual bool await_ready (int timeout_ms) = 0;
    // Runs traffic until `deadline`. `counters` accumulates; the caller resets it
    // at phase boundaries.
    virtual void run (clock_t_::time_point deadline,
                      size_t payload_size,
                      phase_t phase,
                      counters_t &counters,
                      latency_sampler_t *latency) = 0;
    // Threads this driver runs its submit path on. spec 5.1 saturation is judged
    // against the size of this set.
    virtual std::vector<pid_t> submit_threads () = 0;
    virtual const char *implementation () const = 0;
    virtual bool needs_server_counted_throughput () const = 0;
};

// ---------------------------------------------------------------------------
// gRPC driver
//
// grpc++ 1.51.1 async unary over one CompletionQueue, drained on the same thread
// that submits. `window` outstanding calls; window 1 is `request-serial`.
// ---------------------------------------------------------------------------

template <typename TReply> struct grpc_call_t
{
    grpc::ClientContext context;
    TReply reply;
    grpc::Status status;
    std::unique_ptr<grpc::ClientAsyncResponseReader<TReply>> reader;
    uint64_t sent_ns = 0;
};

class grpc_driver_t : public driver_t
{
  public:
    grpc_driver_t (const options_t &options, int window, bool command_path) :
        _options (options), _window (window), _command_path (command_path)
    {
        _channel = grpc::CreateChannel (options.grpc_endpoint, grpc::InsecureChannelCredentials ());
        _stub = zlink_cpp_bench_grpc::BenchService::NewStub (_channel);
    }

    const char *implementation () const override { return "grpc-cpp"; }
    bool needs_server_counted_throughput () const override { return _command_path; }
    std::vector<pid_t> submit_threads () override { return _submit_threads; }

    bool await_ready (int timeout_ms) override
    {
        const auto deadline = clock_t_::now () + std::chrono::milliseconds (timeout_ms);
        while (clock_t_::now () < deadline) {
            grpc::ClientContext context;
            context.set_deadline (std::chrono::system_clock::now () + std::chrono::seconds (2));
            zlink_cpp_bench_grpc::BenchPayload request;
            std::vector<unsigned char> encoded;
            encode_bench_payload (encoded, 1024, 0, phase_warmup, 0);
            const unsigned char *body = nullptr;
            size_t body_size = 0;
            decode_bench_payload_body (encoded.data (), encoded.size (), &body, &body_size);
            request.set_body (body, body_size);
            zlink_cpp_bench_grpc::BenchPayload reply;
            if (_stub->Echo (&context, request, &reply).ok ())
                return true;
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
        return false;
    }

    void run (clock_t_::time_point deadline,
              size_t payload_size,
              phase_t phase,
              counters_t &counters,
              latency_sampler_t *latency) override
    {
        _submit_threads = {current_tid ()};
        if (_command_path)
            run_typed<zlink_cpp_bench_grpc::BenchEmpty> (deadline, payload_size, phase, counters,
                                                         latency, false);
        else
            run_typed<zlink_cpp_bench_grpc::BenchPayload> (deadline, payload_size, phase, counters,
                                                           latency, true);
    }

  private:
    template <typename TReply>
    void run_typed (clock_t_::time_point deadline,
                    size_t payload_size,
                    phase_t phase,
                    counters_t &counters,
                    latency_sampler_t *latency,
                    bool validate_reply)
    {
        grpc::CompletionQueue cq;
        uint64_t seq = 0;
        long long open = 0;
        std::vector<unsigned char> encoded;

        while (clock_t_::now () < deadline || open > 0) {
            while (open < _window && clock_t_::now () < deadline) {
                const size_t body_offset =
                  encode_bench_payload (encoded, payload_size, _run_id, phase, seq++);
                zlink_cpp_bench_grpc::BenchPayload request;
                request.set_body (encoded.data () + body_offset, encoded.size () - body_offset);

                auto *call = new grpc_call_t<TReply> ();
                call->sent_ns = now_ns ();
                call->reader = prepare<TReply> (&call->context, request, &cq);
                call->reader->StartCall ();
                call->reader->Finish (&call->reply, &call->status, static_cast<void *> (call));
                counters.enter ();
                counters.submitted.fetch_add (1, std::memory_order_relaxed);
                ++open;
            }

            void *tag = nullptr;
            bool ok = false;
            const auto next_deadline =
              std::chrono::system_clock::now () + std::chrono::milliseconds (5);
            const auto status = cq.AsyncNext (&tag, &ok, next_deadline);
            if (status == grpc::CompletionQueue::SHUTDOWN)
                break;
            if (status == grpc::CompletionQueue::TIMEOUT)
                continue;

            auto *call = static_cast<grpc_call_t<TReply> *> (tag);
            --open;
            counters.leave ();
            if (ok && call->status.ok ()) {
                bool valid = true;
                if (validate_reply)
                    valid = validate<TReply> (call->reply, phase, counters);
                if (valid) {
                    const uint64_t now = now_ns ();
                    if (latency)
                        latency->add_us (
                          static_cast<double> (now >= call->sent_ns ? now - call->sent_ns : 0)
                          / 1000.0);
                    counters.completed.fetch_add (1, std::memory_order_relaxed);
                }
            } else {
                counters.errors.fetch_add (1, std::memory_order_relaxed);
            }
            delete call;
        }
        cq.Shutdown ();
        void *tag = nullptr;
        bool ok = false;
        while (cq.Next (&tag, &ok))
            delete static_cast<grpc_call_t<TReply> *> (tag);
    }

    template <typename TReply>
    std::unique_ptr<grpc::ClientAsyncResponseReader<TReply>> prepare (
      grpc::ClientContext *context,
      const zlink_cpp_bench_grpc::BenchPayload &request,
      grpc::CompletionQueue *cq);

    // G2: the reply's 29-byte header is validated, and failures are counted
    // rather than silently treated as completions.
    template <typename TReply>
    bool validate (const TReply &reply, phase_t phase, counters_t &counters);

    const options_t &_options;
    int _window;
    bool _command_path;
    uint32_t _run_id = static_cast<uint32_t> (now_ns ());
    std::shared_ptr<grpc::Channel> _channel;
    std::unique_ptr<zlink_cpp_bench_grpc::BenchService::Stub> _stub;
    std::vector<pid_t> _submit_threads;
};

template <>
std::unique_ptr<grpc::ClientAsyncResponseReader<zlink_cpp_bench_grpc::BenchPayload>>
grpc_driver_t::prepare<zlink_cpp_bench_grpc::BenchPayload> (
  grpc::ClientContext *context,
  const zlink_cpp_bench_grpc::BenchPayload &request,
  grpc::CompletionQueue *cq)
{
    return _stub->PrepareAsyncEcho (context, request, cq);
}

template <>
std::unique_ptr<grpc::ClientAsyncResponseReader<zlink_cpp_bench_grpc::BenchEmpty>>
grpc_driver_t::prepare<zlink_cpp_bench_grpc::BenchEmpty> (
  grpc::ClientContext *context,
  const zlink_cpp_bench_grpc::BenchPayload &request,
  grpc::CompletionQueue *cq)
{
    return _stub->PrepareAsyncCommand (context, request, cq);
}

template <>
bool grpc_driver_t::validate<zlink_cpp_bench_grpc::BenchPayload> (
  const zlink_cpp_bench_grpc::BenchPayload &reply, phase_t phase, counters_t &counters)
{
    decoded_header_t header {};
    if (!decode_payload (reply.body ().data (), reply.body ().size (), &header)
        || header.run_id != _run_id || header.phase != static_cast<uint8_t> (phase)) {
        counters.header_failures.fetch_add (1, std::memory_order_relaxed);
        counters.errors.fetch_add (1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

template <>
bool grpc_driver_t::validate<zlink_cpp_bench_grpc::BenchEmpty> (
  const zlink_cpp_bench_grpc::BenchEmpty &, phase_t, counters_t &)
{
    return true;
}

// ---------------------------------------------------------------------------
// ZLink raw driver (`zlink-cpp`)
//
// spec 1.3 / FB-001: ROUTER<->ROUTER. The client creates a ROUTER of its own and
// addresses the server ROUTER by routing id.
// FB-024 / spec 3: two parts on the wire -- a JSON envelope header and a
// hand-encoded protobuf `BenchPayload` -- byte-identical to `zlink-c`, because
// formula 1 divides one by the other.
// ---------------------------------------------------------------------------

// TSocket is `router_socket_t` for the spec 1.3 configuration and
// `dealer_socket_t` for the legacy DEALER->ROUTER one, which stays reachable so
// both can be measured (FB-001). Only the ROUTER form addresses the peer by
// routing id; the operations are otherwise identical, which is the point of
// templating rather than duplicating the driver.
template <typename TSocket> class zlink_raw_driver_impl_t : public driver_t
{
  public:
    static constexpr bool routed = std::is_same_v<TSocket, zlink::router_socket_t>;

    zlink_raw_driver_impl_t (const options_t &options, int window, bool command_path) :
        _options (options), _window (window), _command_path (command_path)
    {
        _socket = std::make_unique<TSocket> (_context);
        const std::string client_rid =
          (command_path ? "zlink-cpp-bench-command-client-" : "zlink-cpp-bench-request-client-")
          + std::to_string (::getpid ());
        if constexpr (routed)
            _socket->set_routing_id (zlink::routing_id_t::from (client_rid));
        _target = zlink::routing_id_t::from (command_path ? options.raw_command_rid
                                                          : options.raw_request_rid);
        _socket->connect (command_path ? options.raw_command_endpoint
                                       : options.raw_request_endpoint);
        _poller.add (*_socket, zlink::poll_event_flag_t::pollcompletion, 1);
    }

    const char *implementation () const override { return "zlink-cpp"; }
    bool needs_server_counted_throughput () const override { return _command_path; }
    std::vector<pid_t> submit_threads () override { return _submit_threads; }

    // Bounded readiness before warmup: a ROUTER's first send races the
    // connection handshake, so the cell would otherwise charge that race to the
    // measurement.
    //
    // The probe runs through the SAME async pump the measured phases use. The
    // blocking `submit()` terminal cannot be used here: this socket is
    // registered with a poller, and the blocking terminal waits for a
    // completion drain that only `poller_t::wait` performs, so it never returns.
    // Driving readiness through the measured path also means readiness proves
    // the path the cell will actually use.
    bool await_ready (int timeout_ms) override
    {
        const auto deadline = clock_t_::now () + std::chrono::milliseconds (timeout_ms);
        while (clock_t_::now () < deadline) {
            counters_t probe;
            const auto attempt_deadline =
              std::min (deadline, clock_t_::now () + std::chrono::milliseconds (500));
            run_slots (attempt_deadline, 1024, phase_warmup, probe, nullptr, 1);
            if (probe.completed.load () > 0)
                return true;
        }
        return false;
    }

    void run (clock_t_::time_point deadline,
              size_t payload_size,
              phase_t phase,
              counters_t &counters,
              latency_sampler_t *latency) override
    {
        run_slots (deadline, payload_size, phase, counters, latency, _window);
    }

    long long abandoned () const { return _abandoned; }

  private:
    void run_slots (clock_t_::time_point deadline,
                    size_t payload_size,
                    phase_t phase,
                    counters_t &counters,
                    latency_sampler_t *latency,
                    int slot_count)
    {
        _submit_threads = {current_tid ()};
        _deadline = deadline;
        _phase = phase;
        _payload_size = payload_size;
        _counters = &counters;
        _latency = latency;

        std::vector<task_t> slots;
        slots.reserve (static_cast<size_t> (slot_count));
        for (int i = 0; i < slot_count; ++i)
            slots.push_back (_command_path ? send_slot () : request_slot ());

        // The application thread: run ready continuations, then let the poller
        // drive the socket-local completion drain. Slots that never complete keep
        // this loop alive only until the drain bound, so a wedged socket is
        // recorded as a wedged cell instead of hanging the run.
        const auto hard_stop = deadline + std::chrono::milliseconds (_options.drain_bound_ms);
        std::vector<zlink::poll_event_t> events (4);
        for (;;) {
            const size_t resumed = _ready.run_ready_round ();
            const bool all_done = std::all_of (slots.begin (), slots.end (),
                                               [] (const task_t &t) { return t.done (); });
            const auto now = clock_t_::now ();
            if (all_done)
                break;
            if (now >= hard_stop)
                break;
            const auto wait_until = now < deadline ? deadline : hard_stop;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
              wait_until - now);
            const std::chrono::milliseconds wait =
              resumed != 0 ? std::chrono::milliseconds (0)
                           : std::chrono::milliseconds (std::max<int64_t> (1, remaining.count ()));
            _poller.wait (events.data (), events.size (), wait);
        }

        // Anything still outstanding when the loop gives up was abandoned
        // (FB-017). It is reported, not swept into `errors` silently.
        _abandoned = counters.outstanding.load (std::memory_order_acquire);
    }

    std::pair<zlink::message_t, zlink::message_t> make_parts (size_t payload_size,
                                                             phase_t phase,
                                                             uint64_t seq)
    {
        const char *envelope = request_envelope ();
        zlink::message_t header = zlink::message_t::from (
          std::as_bytes (std::span<const char> (envelope, std::strlen (envelope))));
        std::vector<unsigned char> encoded;
        encode_bench_payload (encoded, payload_size, _run_id, phase, seq);
        zlink::message_t body = zlink::message_t::from (
          std::span<const uint8_t> (encoded.data (), encoded.size ()));
        return {std::move (header), std::move (body)};
    }

    zlink::request_operation_t request_operation ()
    {
        if constexpr (routed)
            return _socket->request (_target);
        else
            return _socket->request ();
    }

    zlink::send_operation_t send_operation ()
    {
        if constexpr (routed)
            return _socket->send (_target);
        else
            return _socket->send ();
    }

    task_t request_slot ()
    {
        co_await _ready.schedule ();
        while (clock_t_::now () < _deadline) {
            const uint64_t seq = _seq++;
            auto parts = make_parts (_payload_size, _phase, seq);
            _counters->enter ();
            _counters->submitted.fetch_add (1, std::memory_order_relaxed);
            try {
                std::vector<zlink::message_t> reply =
                  co_await std::move (request_operation ())
                    .message (parts.first)
                    .message (parts.second)
                    .timeout (std::chrono::milliseconds (_options.request_timeout_ms))
                    .async ();
                _counters->leave ();
                record_reply (reply);
            }
            catch (const std::exception &) {
                _counters->leave ();
                _counters->errors.fetch_add (1, std::memory_order_relaxed);
            }
        }
    }

    task_t send_slot ()
    {
        co_await _ready.schedule ();
        while (clock_t_::now () < _deadline) {
            const uint64_t seq = _seq++;
            auto parts = make_parts (_payload_size, _phase, seq);
            _counters->enter ();
            _counters->submitted.fetch_add (1, std::memory_order_relaxed);
            try {
                co_await std::move (send_operation ())
                  .message (parts.first)
                  .message (parts.second)
                  .async ();
                _counters->leave ();
                _counters->completed.fetch_add (1, std::memory_order_relaxed);
            }
            catch (const std::exception &) {
                _counters->leave ();
                _counters->errors.fetch_add (1, std::memory_order_relaxed);
            }
        }
    }

    // G2: validate the returned 29-byte header before counting a completion.
    void record_reply (const std::vector<zlink::message_t> &reply)
    {
        if (reply.empty ()) {
            _counters->errors.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        const zlink::message_t &body = reply.back ();
        const unsigned char *payload = nullptr;
        size_t payload_size = 0;
        decoded_header_t header {};
        if (!decode_bench_payload_body (static_cast<const void *> (body.data ()), body.size (),
                                        &payload, &payload_size)
            || !decode_payload (payload, payload_size, &header) || header.run_id != _run_id) {
            _counters->header_failures.fetch_add (1, std::memory_order_relaxed);
            _counters->errors.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        const uint64_t now = now_ns ();
        if (_latency)
            _latency->add_us (
              static_cast<double> (now >= header.sent_ns ? now - header.sent_ns : 0) / 1000.0);
        _counters->completed.fetch_add (1, std::memory_order_relaxed);
    }

    const options_t &_options;
    int _window;
    bool _command_path;
    uint32_t _run_id = static_cast<uint32_t> (now_ns ());
    zlink::context_t _context;
    std::unique_ptr<TSocket> _socket;
    zlink::routing_id_t _target = zlink::routing_id_t::from (std::string ("unset"));
    zlink::poller_t _poller;
    ready_queue_t _ready;

    clock_t_::time_point _deadline {};
    phase_t _phase = phase_warmup;
    size_t _payload_size = 1024;
    counters_t *_counters = nullptr;
    latency_sampler_t *_latency = nullptr;
    uint64_t _seq = 0;
    long long _abandoned = 0;
    std::vector<pid_t> _submit_threads;
};

using zlink_raw_driver_t = zlink_raw_driver_impl_t<zlink::router_socket_t>;
using zlink_raw_dealer_driver_t = zlink_raw_driver_impl_t<zlink::dealer_socket_t>;

// ---------------------------------------------------------------------------
// the shared cell runner
// ---------------------------------------------------------------------------

struct cell_request_t
{
    std::string implementation;
    std::string pattern;
    size_t payload_size = 0;
    int window = 1;
    server_endpoint_t server;
    bool server_counted_throughput = false;
};

class cell_runner_t
{
  public:
    explicit cell_runner_t (const options_t &options) : _options (options) {}

    // Marks a server contaminated so the NEXT cell that uses it is excluded from
    // the tables and every judgement (FB-008, spec 3). Contaminated cells are
    // still measured and still recorded; they are just not usable.
    void mark_contaminated (const std::string &server_name, const std::string &reason)
    {
        _contaminated[server_name] = reason;
    }

    cell_t run (driver_t &driver, const cell_request_t &request)
    {
        cell_t cell;
        cell.implementation = request.implementation;
        cell.pattern = request.pattern;
        cell.payload_size = request.payload_size;
        cell.request_window = request.window;
        cell.logical_cores_value = logical_cores ();
        cell.client_parallelism_ceiling = 1.0; // one application thread; see the file header
        cell.client_saturation_metric = "submit_thread_cores";

        const auto contamination = _contaminated.find (request.server.name);
        if (contamination != _contaminated.end ()) {
            cell.contaminated = true;
            cell.contamination_reason = contamination->second;
            _contaminated.erase (contamination);
        }

        counters_t counters;
        latency_sampler_t latency (_options.latency_sample_limit);

        // 1. readiness, outside every measured window
        if (!driver.await_ready (_options.readiness_timeout_ms)) {
            cell.contaminated = true;
            cell.contamination_reason = "route readiness timed out before warmup";
            return cell;
        }

        // 2. warmup, sampled per segment so the warmup length is justified from
        //    this run's own evidence (spec 8.2) rather than copied from another
        //    language's runtime.
        run_warmup (driver, request, counters, cell);

        // 3. open the measured window
        counters.reset ();
        latency.reset ();
        reset_stats (request.server.host, request.server.port);

        const auto submit_before = submit_cpu (driver.submit_threads ());
        const double process_before = process_cpu_seconds_self ();
        const auto server_before = fetch_stats (request.server.host, request.server.port);
        const double server_cpu_before = server_before ? server_before->cpu_seconds : 0.0;

        const auto start = clock_t_::now ();
        const auto deadline = start + std::chrono::duration_cast<clock_t_::duration> (
                                        std::chrono::duration<double> (_options.duration_seconds));
        driver.run (deadline, request.payload_size, phase_active, counters, &latency);
        const auto stop = clock_t_::now ();

        // 4. FB-013: the boundary sample. `send-saturation` throughput is what
        //    the server had consumed when the active window closed, never what a
        //    later drain-filtered read shows.
        const auto boundary = fetch_stats (request.server.host, request.server.port);
        const double process_after = process_cpu_seconds_self ();
        const auto submit_after = submit_cpu (driver.submit_threads ());

        const double elapsed = std::chrono::duration<double> (stop - start).count ();

        // 5. FB-008: drain confirmation with a bound.
        const auto drain = drain_confirmed (request.server, _options.drain_bound_ms);
        cell.drain_ms = drain.drain_ms;
        cell.drain_bound_hit = drain.bound_hit;
        if (drain.bound_hit)
            mark_contaminated (request.server.name,
                               "previous cell on this server did not drain within "
                                 + std::to_string (_options.drain_bound_ms) + " ms");

        const auto after_drain = fetch_stats (request.server.host, request.server.port);

        cell.completed = counters.completed.load ();
        cell.errors = counters.errors.load ();
        cell.submitted = counters.submitted.load ();
        cell.header_validation_failures = counters.header_failures.load ();
        cell.peak_in_flight = counters.peak_in_flight.load ();
        cell.abandoned = counters.outstanding.load ();

        if (boundary) {
            cell.server_received_at_close = boundary->active_messages;
            cell.server_memory_mb = boundary->rss_mb;
            cell.server_cpu_percent = (boundary->cpu_seconds - server_cpu_before)
                                      / std::max (0.001, elapsed)
                                      / static_cast<double> (cell.logical_cores_value) * 100.0;
        }
        if (after_drain)
            cell.server_received_post_drain = after_drain->active_messages;

        // spec 5 / G3: a send cell's throughput is the SERVER's received count.
        const double completions =
          request.server_counted_throughput
            ? static_cast<double> (cell.server_received_at_close.value_or (0))
            : static_cast<double> (cell.completed);
        cell.throughput_per_second = completions / std::max (0.001, elapsed);
        cell.bandwidth_mb_s =
          cell.throughput_per_second * static_cast<double> (request.payload_size) / 1e6;

        if (request.server_counted_throughput && boundary) {
            // spec 5: a send cell's latency is the server-side receive latency it
            // computed from the header, not a client stopwatch.
            cell.latency_mean_ms = boundary->mean_us / 1000.0;
            cell.latency_p95_ms = boundary->p95_us / 1000.0;
            cell.latency_p99_ms = boundary->p99_us / 1000.0;
        } else {
            cell.latency_mean_ms = latency.mean_us () / 1000.0;
            cell.latency_p95_ms = latency.percentile (0.95) / 1000.0;
            cell.latency_p99_ms = latency.percentile (0.99) / 1000.0;
        }

        const double process_cores = (process_after - process_before) / std::max (0.001, elapsed);
        const double submit_cores = (submit_after - submit_before) / std::max (0.001, elapsed);
        cell.client_cores = process_cores;
        cell.submit_thread_cores = submit_cores;
        cell.non_submit_cores = std::max (0.0, process_cores - submit_cores);
        cell.client_cpu_percent =
          process_cores / static_cast<double> (cell.logical_cores_value) * 100.0;
        cell.client_memory_mb = rss_mb ();
        cell.client_threads = thread_count_self ();
        return cell;
    }

  private:
    static double submit_cpu (const std::vector<pid_t> &threads)
    {
        double total = 0.0;
        for (const pid_t tid : threads)
            total += thread_cpu_seconds_tid (tid);
        return total;
    }

    void run_warmup (driver_t &driver,
                     const cell_request_t &request,
                     counters_t &counters,
                     cell_t &cell)
    {
        if (_options.warmup_seconds <= 0.0)
            return;
        const int segments = std::max (1, _options.warmup_segments);
        const double segment_seconds = _options.warmup_seconds / static_cast<double> (segments);
        for (int i = 0; i < segments; ++i) {
            counters.reset ();
            const auto start = clock_t_::now ();
            const auto deadline =
              start + std::chrono::duration_cast<clock_t_::duration> (
                        std::chrono::duration<double> (segment_seconds));
            driver.run (deadline, request.payload_size, phase_warmup, counters, nullptr);
            const double elapsed =
              std::chrono::duration<double> (clock_t_::now () - start).count ();
            const long long done = request.server_counted_throughput
                                     ? counters.submitted.load ()
                                     : counters.completed.load ();
            cell.warmup_segment_throughput.push_back (static_cast<double> (done)
                                                      / std::max (0.001, elapsed));
        }
    }

    const options_t &_options;
    std::map<std::string, std::string> _contaminated;
};

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main (int argc, char **argv)
{
    options_t options;
    options.grpc_endpoint = arg_value (argc, argv, "--grpc-endpoint", options.grpc_endpoint.c_str ());
    options.grpc_stats_port =
      std::atoi (arg_value (argc, argv, "--grpc-stats-port", "5114").c_str ());
    options.framework_endpoint =
      arg_value (argc, argv, "--framework-endpoint", options.framework_endpoint.c_str ());
    options.framework_stats_port =
      std::atoi (arg_value (argc, argv, "--framework-stats-port", "5113").c_str ());
    options.raw_request_endpoint =
      arg_value (argc, argv, "--raw-request-endpoint", options.raw_request_endpoint.c_str ());
    options.raw_command_endpoint =
      arg_value (argc, argv, "--raw-command-endpoint", options.raw_command_endpoint.c_str ());
    options.raw_stats_port = std::atoi (arg_value (argc, argv, "--raw-stats-port", "5116").c_str ());
    options.raw_socket = arg_value (argc, argv, "--raw-socket", options.raw_socket.c_str ());
    options.payload_sizes = parse_sizes (arg_value (argc, argv, "--payload-sizes", "1024,4096"));
    options.implementations = split_csv (
      arg_value (argc, argv, "--implementations", "grpc-cpp,zlink-cpp,zlink-framework-cpp"));
    options.patterns = split_csv (
      arg_value (argc, argv, "--patterns", "request-serial,request-window,send-saturation"));
    options.duration_seconds = std::atof (arg_value (argc, argv, "--duration-seconds", "5").c_str ());
    options.warmup_seconds = std::atof (arg_value (argc, argv, "--warmup", "5").c_str ());
    options.warmup_segments = std::atoi (arg_value (argc, argv, "--warmup-segments", "10").c_str ());
    options.request_window = std::atoi (arg_value (argc, argv, "--request-window", "100").c_str ());
    options.send_concurrency = std::atoi (arg_value (argc, argv, "--send-concurrency", "8").c_str ());
    options.request_timeout_ms =
      std::atoi (arg_value (argc, argv, "--request-timeout-ms", "30000").c_str ());
    options.drain_bound_ms = std::atoi (arg_value (argc, argv, "--drain-bound-ms", "30000").c_str ());
    options.output_dir = arg_value (argc, argv, "--output-dir", "log/adhoc");
    options.run_label = arg_value (argc, argv, "--run-label", "cpp-router-1");

    ::mkdir (options.output_dir.c_str (), 0755);

    const server_endpoint_t grpc_server {options.host, options.grpc_stats_port, "grpc"};
    const server_endpoint_t raw_server {options.host, options.raw_stats_port, "raw"};
    const server_endpoint_t framework_server {options.host, options.framework_stats_port,
                                              "framework"};

    std::fprintf (stderr, "[bench] cpp client start label=%s loadavg1=%.2f\n",
                  options.run_label.c_str (), loadavg1 ());

    cell_runner_t runner (options);
    std::vector<cell_t> cells;
    std::vector<std::string> failures;

    // Cell isolation (plan "Phase 2~5 공통 요구"): one cell that throws must not
    // take the other seventeen with it, which is what happened in Phase 0.
    auto run_cell = [&] (const std::string &implementation, const std::string &pattern,
                         size_t payload_size) {
        const bool command_path = pattern == "send-saturation";
        const int window = pattern == "request-serial"  ? 1
                           : pattern == "request-window" ? options.request_window
                                                         : options.send_concurrency;
        cell_request_t request;
        request.implementation = implementation;
        request.pattern = pattern;
        request.payload_size = payload_size;
        request.window = window;
        request.server_counted_throughput = command_path;

        try {
            std::unique_ptr<driver_t> driver;
            if (implementation == "grpc-cpp") {
                request.server = grpc_server;
                driver = std::make_unique<grpc_driver_t> (options, window, command_path);
            } else if (implementation == "zlink-cpp") {
                request.server = raw_server;
                if (options.raw_socket == "dealer")
                    driver = std::make_unique<zlink_raw_dealer_driver_t> (options, window,
                                                                          command_path);
                else
                    driver = std::make_unique<zlink_raw_driver_t> (options, window, command_path);
            } else {
                failures.push_back (implementation + "-" + pattern + "@"
                                    + std::to_string (payload_size) + ": not implemented");
                return;
            }
            cell_t cell = runner.run (*driver, request);
            if (auto *raw = dynamic_cast<zlink_raw_driver_t *> (driver.get ()))
                cell.abandoned = std::max (cell.abandoned, raw->abandoned ());
            if (auto *raw = dynamic_cast<zlink_raw_dealer_driver_t *> (driver.get ()))
                cell.abandoned = std::max (cell.abandoned, raw->abandoned ());
            std::fprintf (stderr,
                          "[bench] %-28s @%-5zu tput=%10.1f/s depth=%6.2f peak=%4lld "
                          "abandoned=%4lld errors=%6lld drain=%7.0fms submit_cores=%.3f "
                          "process_cores=%.3f\n",
                          cell.scenario ().c_str (), cell.payload_size,
                          cell.throughput_per_second,
                          cell.throughput_per_second * cell.latency_mean_ms / 1000.0,
                          cell.peak_in_flight, cell.abandoned, cell.errors,
                          cell.drain_ms.value_or (0.0), cell.submit_thread_cores,
                          cell.client_cores);
            std::fflush (stderr);
            cells.push_back (std::move (cell));
        }
        catch (const std::exception &error) {
            failures.push_back (implementation + "-" + pattern + "@"
                                + std::to_string (payload_size) + ": " + error.what ());
            std::fprintf (stderr, "[bench] CELL FAILED %s-%s@%zu: %s\n", implementation.c_str (),
                          pattern.c_str (), payload_size, error.what ());
            std::fflush (stderr);
        }
    };

    for (const std::string &pattern : options.patterns)
        for (const size_t payload_size : options.payload_sizes)
            for (const std::string &implementation : options.implementations)
                run_cell (implementation, pattern, payload_size);

    // report.txt: the spec 4 table and RESULT lines, for reading one run by eye.
    // Never the basis of a judgement (spec 7.4, FB-020).
    const std::string report_path = options.output_dir + "/report.txt";
    if (std::FILE *report = std::fopen (report_path.c_str (), "w")) {
        std::fprintf (report, "options: raw_socket=%s window=%d send_concurrency=%d "
                              "duration=%.1f warmup=%.1f logical_cores=%ld "
                              "client_parallelism_ceiling=1 client_saturation_metric=%s "
                              "grpc_version=%s loadavg1=%.2f\n",
                      options.raw_socket.c_str (), options.request_window,
                      options.send_concurrency, options.duration_seconds, options.warmup_seconds,
                      logical_cores (), "submit_thread_cores", grpc::Version ().c_str (),
                      loadavg1 ());
        for (const cell_t &cell : cells)
            print_table_row (report, cell);
        for (const cell_t &cell : cells)
            print_result_lines (report, cell);
        std::fclose (report);
    }

    std::map<std::string, std::string> metadata;
    metadata["diagnosticsSchema"] = "with-grpc-cell-v1";
    metadata["language"] = "cpp";
    metadata["grpcCppVersion"] = grpc::Version ();
    metadata["protobufVersion"] = std::to_string (GOOGLE_PROTOBUF_VERSION);
    metadata["grpcServerConfiguration"] =
      "grpc::ServerBuilder synchronous server, InsecureServerCredentials, plaintext loopback, "
      "no option overridden (grpc++ " + grpc::Version () + ")";
    metadata["cpu"] = cpu_model ();
    metadata["logical_cores"] = std::to_string (logical_cores ());
    metadata["client_saturation_metric"] = "submit_thread_cores";
    metadata["client_parallelism_ceiling"] = "1 (one application thread runs submit and drain)";
    metadata["rawSocket"] = options.raw_socket;
    metadata["warmupSeconds"] = std::to_string (options.warmup_seconds);
    metadata["durationSeconds"] = std::to_string (options.duration_seconds);
    metadata["requestWindow"] = std::to_string (options.request_window);
    metadata["sendConcurrency"] = std::to_string (options.send_concurrency);
    metadata["requestTimeoutMs"] = std::to_string (options.request_timeout_ms);
    metadata["runLabel"] = options.run_label;
    metadata["loadavg1"] = std::to_string (loadavg1 ());
    {
        std::string joined;
        for (const std::string &failure : failures)
            joined += (joined.empty () ? "" : "; ") + failure;
        metadata["failedCells"] = joined;
    }

    write_cells_json (options.output_dir + "/cells.json", cells, metadata);
    std::fprintf (stderr, "[bench] cells completed=%zu failed=%zu -> %s/cells.json\n",
                  cells.size (), failures.size (), options.output_dir.c_str ());
    return failures.empty () ? 0 : 1;
}
