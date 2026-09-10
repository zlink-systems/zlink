/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// Source A: HTTP-triggered logical streams. The runner owns process isolation and settle.
#include "../common/bench_async.hpp"
#include "../common/bench_common.hpp"
#include "../common/bench_stats_server.hpp"
#include <zlink/codecs/protobuf.hpp>
#include <zlink/framework.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <csignal>
#include <condition_variable>

#include "bench.grpc.pb.h"
#include "bench.pb.h"

#include <grpcpp/grpcpp.h>

#include <zlink.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <sys/stat.h>

using namespace zlink_cpp_bench;
using clock_t_ = std::chrono::steady_clock;

namespace
{

class completion_signal_t
{
  public:
    void notify (size_t slot)
    {
        {
            std::lock_guard<std::mutex> lock (_mutex);
            _completed.push_back (slot);
        }
        _ready.notify_one ();
    }

    void take_completed (std::vector<size_t> &completed)
    {
        std::lock_guard<std::mutex> lock (_mutex);
        completed.assign (_completed.begin (), _completed.end ());
        _completed.clear ();
    }

    void wait_until (clock_t_::time_point deadline)
    {
        std::unique_lock<std::mutex> lock (_mutex);
        _ready.wait_until (lock, deadline, [&] { return !_completed.empty (); });
    }

  private:
    std::mutex _mutex;
    std::condition_variable _ready;
    std::deque<size_t> _completed;
};


struct options_t
{
    std::string implementation = "grpc-cpp";
    std::string pattern = "request-serial";
    size_t payload_size = 1024;
    std::string grpc_endpoint;
    std::string framework_endpoint;
    std::string raw_request_endpoint;
    std::string raw_command_endpoint;
    std::string raw_request_rid = "zlink-cpp-bench-request-server";
    std::string raw_command_rid = "zlink-cpp-bench-command-server";
    int trigger_port = 5280;
    int stats_port = 5281;
    int target_stats_port = 5283;
    double warmup_seconds = 5;
    int warmup_segments = 10;
    int request_timeout_ms = 30000;
    int drain_bound_ms = 30000;
    int readiness_timeout_ms = 30000;
    size_t latency_sample_limit = 200000;
    std::string output_file;
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

class driver_t
{
  public:
    virtual ~driver_t () = default;
    std::function<void ()> boundary;
    void close_window (clock_t_::time_point deadline)
    {
        if (boundary && clock_t_::now () >= deadline) {
            auto sample = std::move (boundary);
            boundary = {};
            sample ();
        }
    }
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
    size_t stream = 0;
    uint64_t seq = 0;
    size_t payload_size = 0;
};

class grpc_driver_t : public driver_t
{
  public:
    grpc_driver_t (const options_t &options, int window, bool command_path) :
        _options (options), _window (window), _command_path (command_path)
    {
        _channel = grpc::CreateChannel (options.grpc_endpoint, grpc::InsecureChannelCredentials ());
        for (int i = 0; i < (command_path ? 8 : 1); ++i)
            _stubs.push_back (zlink::framework::bench::withgrpc::BenchService::NewStub (_channel));
    }


    bool await_ready (int timeout_ms) override
    {
        const auto deadline = clock_t_::now () + std::chrono::milliseconds (timeout_ms);
        while (clock_t_::now () < deadline) {
            grpc::ClientContext context;
            context.set_deadline (std::chrono::system_clock::now () + std::chrono::seconds (2));
            zlink::framework::bench::withgrpc::BenchPayload request;
            request.mutable_body ()->assign (1024, '\xab');
            stamp_payload (request.mutable_body ()->data (), 1024, 0, phase_warmup, 0);
            zlink::framework::bench::withgrpc::BenchPayload reply;
            if (_stubs.front ()->Echo (&context, request, &reply).ok ())
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
        if (_command_path)
            run_typed<google::protobuf::Empty> (deadline, payload_size, phase, counters,
                                                         latency, false);
        else
            run_typed<zlink::framework::bench::withgrpc::BenchPayload> (deadline, payload_size, phase, counters,
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
        std::vector<bool> busy (_stubs.size (), false);

        // spec 2 request-backpressure: _window <= 0 means no application
        // ceiling. gRPC returns no admission refusal to the caller, so this is
        // the cooperative-yield variant -- submit, then hand control to the
        // completion pump, and let depth settle where the submission and
        // completion rates balance rather than at a number the harness chose.
        const bool uncapped = _window <= 0;

        auto submit_one = [&] {
            zlink::framework::bench::withgrpc::BenchPayload request;
            request.mutable_body ()->assign (payload_size, '\xab');
            stamp_payload (request.mutable_body ()->data (), payload_size, _run_id, phase, seq++);

            auto *call = new grpc_call_t<TReply> ();
            call->seq = seq - 1;
            call->payload_size = payload_size;
            call->context.set_deadline (std::chrono::system_clock::now ()
              + std::chrono::milliseconds (_options.request_timeout_ms));
            if (_command_path) {
                call->stream = static_cast<size_t> (std::find (busy.begin (), busy.end (), false) - busy.begin ());
                busy[call->stream] = true;
            }
            _submit_stream = call->stream;
            call->sent_ns = now_ns ();
            call->reader = prepare<TReply> (&call->context, request, &cq);
            call->reader->StartCall ();
            call->reader->Finish (&call->reply, &call->status, static_cast<void *> (call));
            counters.enter ();
            counters.submitted.fetch_add (1, std::memory_order_relaxed);
            ++open;
        };

        auto handle_completion = [&] (void *tag, bool ok) {
            auto *call = static_cast<grpc_call_t<TReply> *> (tag);
            --open;
            if (_command_path) busy[call->stream] = false;
            counters.leave ();
            if (ok && call->status.ok ()) {
                bool valid = true;
                if (validate_reply)
                    valid = validate<TReply> (call->reply, phase, counters, call->seq, call->payload_size);
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
        };

        // Drains every completion that is ALREADY ready without blocking. This
        // is the cooperative yield: submission never waits on a timeout, but it
        // never runs without letting finished replies be counted either.
        auto drain_ready = [&] {
            for (;;) {
                void *tag = nullptr;
                bool ok = false;
                // The epoch is unambiguously in the past, so this returns
                // TIMEOUT at once when nothing is ready. Computing "now" here
                // instead let the call wait for a round trip, which held the
                // row at depth 1 and made it a serial cell by accident.
                const auto status = cq.AsyncNext (
                  &tag, &ok, std::chrono::system_clock::time_point {});
                if (status != grpc::CompletionQueue::GOT_EVENT)
                    return status != grpc::CompletionQueue::SHUTDOWN;
                handle_completion (tag, ok);
            }
        };

        bool live = true;
        while (live && (clock_t_::now () < deadline || open > 0)) {
            close_window (deadline);
            if (uncapped) {
                if (clock_t_::now () < deadline)
                    submit_one ();
                live = drain_ready ();
                if (clock_t_::now () < deadline)
                    continue;
            } else {
                while (open < _window && clock_t_::now () < deadline)
                    submit_one ();
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
            handle_completion (tag, ok);
        }
        close_window (deadline);
        cq.Shutdown ();
        void *tag = nullptr;
        bool ok = false;
        while (cq.Next (&tag, &ok))
            delete static_cast<grpc_call_t<TReply> *> (tag);
    }

    template <typename TReply>
    std::unique_ptr<grpc::ClientAsyncResponseReader<TReply>> prepare (
      grpc::ClientContext *context,
      const zlink::framework::bench::withgrpc::BenchPayload &request,
      grpc::CompletionQueue *cq);

    // G2: the reply's 29-byte header is validated, and failures are counted
    // rather than silently treated as completions.
    template <typename TReply>
    bool validate (const TReply &reply, phase_t phase, counters_t &counters, uint64_t seq, size_t size);

    const options_t &_options;
    int _window;
    bool _command_path;
    uint32_t _run_id = static_cast<uint32_t> (now_ns ());
    std::shared_ptr<grpc::Channel> _channel;
    std::vector<std::unique_ptr<zlink::framework::bench::withgrpc::BenchService::Stub>> _stubs;
    size_t _submit_stream = 0;
};

template <>
std::unique_ptr<grpc::ClientAsyncResponseReader<zlink::framework::bench::withgrpc::BenchPayload>>
grpc_driver_t::prepare<zlink::framework::bench::withgrpc::BenchPayload> (
  grpc::ClientContext *context,
  const zlink::framework::bench::withgrpc::BenchPayload &request,
  grpc::CompletionQueue *cq)
{
    return _stubs[_submit_stream]->PrepareAsyncEcho (context, request, cq);
}

template <>
std::unique_ptr<grpc::ClientAsyncResponseReader<google::protobuf::Empty>>
grpc_driver_t::prepare<google::protobuf::Empty> (
  grpc::ClientContext *context,
  const zlink::framework::bench::withgrpc::BenchPayload &request,
  grpc::CompletionQueue *cq)
{
    return _stubs[_submit_stream]->PrepareAsyncCommand (context, request, cq);
}

template <>
bool grpc_driver_t::validate<zlink::framework::bench::withgrpc::BenchPayload> (
  const zlink::framework::bench::withgrpc::BenchPayload &reply, phase_t phase, counters_t &counters, uint64_t seq, size_t size)
{
    decoded_header_t header {};
    if (!decode_payload (reply.body ().data (), reply.body ().size (), &header)
        || header.run_id != _run_id || header.phase != static_cast<uint8_t> (phase) || header.seq != seq
        || header.payload_size != size || reply.body ().size () != size) {
        counters.header_failures.fetch_add (1, std::memory_order_relaxed);
        counters.errors.fetch_add (1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

template <>
bool grpc_driver_t::validate<google::protobuf::Empty> (
  const google::protobuf::Empty &, phase_t, counters_t &, uint64_t, size_t)
{
    return true;
}

// ---------------------------------------------------------------------------
// ZLink raw driver (`zlink-cpp`)
//
// spec 1.3 / FB-001: ROUTER<->ROUTER. The client creates a ROUTER of its own and
// addresses the server ROUTER by routing id.
// FB-024 / spec 3: two parts on the wire -- a JSON envelope header and a
// typed protobuf `BenchPayload`, serialized for every message (Issue #66).
// ---------------------------------------------------------------------------

class zlink_raw_driver_t : public driver_t
{
  public:
    zlink_raw_driver_t (const options_t &options, int window, bool command_path) :
        _options (options), _window (window), _command_path (command_path)
    {
        _socket = std::make_unique<zlink::router_socket_t> (_context);
        const std::string client_rid =
          (command_path ? "zlink-cpp-bench-command-client-" : "zlink-cpp-bench-request-client-")
          + std::to_string (::getpid ());
        _socket->set_routing_id (zlink::routing_id_t::from (client_rid));
        _target = zlink::routing_id_t::from (command_path ? options.raw_command_rid
                                                          : options.raw_request_rid);
        _socket->connect (command_path ? options.raw_command_endpoint
                                       : options.raw_request_endpoint);
        _poller.add (*_socket, zlink::poll_event_flag_t::pollcompletion, 1);
    }


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
        // spec 2 request-backpressure: _window <= 0 means no application
        // ceiling, so depth cannot be a slot count.
        if (_window <= 0)
            run_unbounded (deadline, payload_size, phase, counters, latency);
        else
            run_slots (deadline, payload_size, phase, counters, latency, _window);
    }

    void run_unbounded (clock_t_::time_point deadline,
                        size_t payload_size,
                        phase_t phase,
                        counters_t &counters,
                        latency_sampler_t *latency)
    {
        _deadline = deadline;
        _phase = phase;
        _payload_size = payload_size;
        _counters = &counters;
        _latency = latency;

        std::vector<task_t> replies;
        task_t submitter = request_submitter (replies);
        const auto hard_stop = deadline + std::chrono::milliseconds (_options.drain_bound_ms);
        std::vector<zlink::poll_event_t> events (4);
        for (;;) {
            const auto now = clock_t_::now ();
            close_window (deadline);
            const size_t resumed = _ready.run_ready_round ();
            replies.erase (std::remove_if (replies.begin (), replies.end (),
                                           [] (const task_t &t) { return t.done (); }),
                           replies.end ());

            if (submitter.done () && replies.empty ())
                break;
            if (now >= hard_stop)
                break;

            const std::chrono::milliseconds wait =
              resumed != 0 ? std::chrono::milliseconds (0)
                           : std::chrono::milliseconds (1);
            _poller.wait (events.data (), events.size (), wait);
        }

        close_window (deadline);
    }


  private:
    void run_slots (clock_t_::time_point deadline,
                    size_t payload_size,
                    phase_t phase,
                    counters_t &counters,
                    latency_sampler_t *latency,
                    int slot_count)
    {
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
            close_window (deadline);
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
        close_window (deadline);
    }

    std::pair<zlink::message_t, zlink::message_t> make_parts (size_t payload_size,
                                                             phase_t phase,
                                                             uint64_t seq)
    {
        const char *envelope = request_envelope ();
        zlink::message_t header = zlink::message_t::from (
          std::as_bytes (std::span<const char> (envelope, std::strlen (envelope))));
        const size_t body_size = std::max (payload_size, k_header_size);
        zlink::framework::bench::withgrpc::BenchPayload payload;
        payload.mutable_body ()->assign (body_size, '\xab');
        stamp_payload (payload.mutable_body ()->data (), body_size, _run_id, phase, seq);
        const std::string encoded = encode_bench_payload (payload);
        zlink::message_t body = zlink::message_t::from (
          std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));
        return {std::move (header), std::move (body)};
    }

    zlink::request_operation_t request_operation ()
    {
        return _socket->request (_target);
    }

    zlink::send_operation_t send_operation ()
    {
        return _socket->send (_target);
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
                zlink::request_submission_t submission =
                  request_operation ()
                    .message (parts.first)
                    .message (parts.second)
                    .timeout (std::chrono::milliseconds (_options.request_timeout_ms))
                    .async ();
                if (submission.result == ZLINK_SUBMIT_BACKPRESSURED)
                    co_await std::move (submission.admitted);
                else if (submission.result != ZLINK_SUBMIT_OK)
                    throw std::logic_error ("unexpected request submit result");
                std::vector<zlink::message_t> reply =
                  co_await std::move (submission.reply);
                _counters->leave ();
                record_reply (reply, seq);
            }
            catch (const std::exception &error) {
                if (_counters->errors.load () == 0) std::fprintf (stderr, "raw operation failed: %s\n", error.what ());
                _counters->leave ();
                _counters->errors.fetch_add (1, std::memory_order_relaxed);
            }
        }
    }

    task_t observe_request_reply (zlink::async_result_t<std::vector<zlink::message_t>> reply_,
                                  uint64_t seq)
    {
        co_await _ready.schedule ();
        try {
            std::vector<zlink::message_t> reply =
              co_await std::move (reply_);
            _counters->leave ();
            record_reply (reply, seq);
        }
        catch (const std::exception &error) {
                if (_counters->errors.load () == 0) std::fprintf (stderr, "raw operation failed: %s\n", error.what ());
            _counters->leave ();
            _counters->errors.fetch_add (1, std::memory_order_relaxed);
        }
    }

    task_t request_submitter (std::vector<task_t> &replies)
    {
        co_await _ready.schedule ();
        size_t submitted_since_progress = 0;
        while (clock_t_::now () < _deadline) {
            const uint64_t seq = _seq++;
            auto parts = make_parts (_payload_size, _phase, seq);
            _counters->enter ();
            _counters->submitted.fetch_add (1, std::memory_order_relaxed);
            try {
                zlink::request_submission_t submission =
                  request_operation ()
                    .message (parts.first)
                    .message (parts.second)
                    .timeout (std::chrono::milliseconds (_options.request_timeout_ms))
                    .async ();
                const bool backpressured =
                  submission.result == ZLINK_SUBMIT_BACKPRESSURED;
                if (submission.result != ZLINK_SUBMIT_OK && !backpressured)
                    throw std::logic_error ("unexpected request submit result");
                replies.push_back (
                  observe_request_reply (std::move (submission.reply), seq));
                if (backpressured) {
                    try {
                        co_await std::move (submission.admitted);
                    }
                    catch (...) {
                        co_return;
                    }
                }
            }
            catch (const std::exception &error) {
                if (_counters->errors.load () == 0)
                    std::fprintf (stderr, "raw operation failed: %s\n", error.what ());
                _counters->leave ();
                _counters->errors.fetch_add (1, std::memory_order_relaxed);
                co_await _ready.schedule ();
                continue;
            }
            if (++submitted_since_progress == 64) {
                submitted_since_progress = 0;
                co_await _ready.schedule ();
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
                zlink::send_submission_t submission = send_operation ()
                  .message (parts.first)
                  .message (parts.second)
                  .async ();
                if (submission.result == ZLINK_SUBMIT_BACKPRESSURED)
                    co_await std::move (submission.admitted);
                else if (submission.result != ZLINK_SUBMIT_OK)
                    throw std::logic_error ("unexpected send submit result");
                _counters->leave ();
                _counters->completed.fetch_add (1, std::memory_order_relaxed);
            }
            catch (const std::exception &error) {
                if (_counters->errors.load () == 0) std::fprintf (stderr, "raw operation failed: %s\n", error.what ());
                _counters->leave ();
                _counters->errors.fetch_add (1, std::memory_order_relaxed);
            }
        }
    }

    // G2: validate the returned 29-byte header before counting a completion.
    void record_reply (const std::vector<zlink::message_t> &reply, uint64_t seq)
    {
        if (reply.empty ()) {
            _counters->errors.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        const zlink::message_t &body = reply.back ();
        zlink::framework::bench::withgrpc::BenchPayload payload;
        decoded_header_t header {};
        if (!payload.ParseFromArray (body.data (), static_cast<int> (body.size ()))
            || !decode_payload (payload.body ().data (), payload.body ().size (), &header)
            || header.run_id != _run_id || header.phase != _phase
            || header.payload_size != _payload_size || payload.body ().size () != _payload_size || header.seq != seq) {
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
    std::unique_ptr<zlink::router_socket_t> _socket;
    zlink::routing_id_t _target = zlink::routing_id_t::from (std::string ("unset"));
    zlink::poller_t _poller;
    ready_queue_t _ready;

    clock_t_::time_point _deadline {};
    phase_t _phase = phase_warmup;
    size_t _payload_size = 1024;
    counters_t *_counters = nullptr;
    latency_sampler_t *_latency = nullptr;
    uint64_t _seq = 0;
};


namespace fw = zlink::framework;
using payload_t = zlink::framework::bench::withgrpc::BenchPayload;
using json = nlohmann::json;

class framework_driver_t final : public driver_t
{
  public:
    framework_driver_t (const options_t &options, int window, bool command) :
        _options (options), _window (window), _command (command),
        _host (std::vector<int>{}, bench_http_callback_t{})
    {
        auto &app = _host.app ();
        auto &config = app.add_zlink_framework ();
        config.codecs ().use (zlink::framework_codecs::protobuf ());
        auto mesh = config.add_route_mesh ("bench");
        mesh.set_object_role (fw::object_role_t::none).listen ("tcp://127.0.0.1:0").set_routing_id (zlink::routing_id_t::from ("bench-source"));
        mesh.channel ("bench").client ();
        mesh.peer_connections ().connect (zlink::routing_id_t::from ("bench-server"), options.framework_endpoint);
        app.add_hosted_service (std::make_unique<capture_client_t> (_client));
    }


    bool await_ready (int timeout_ms) override
    {
        if (!_host.start ()) return false;
        const auto deadline = clock_t_::now () + std::chrono::milliseconds (timeout_ms);
        while (clock_t_::now () < deadline) {
            auto payload = make_payload (1024, phase_warmup);
            auto task = _client->request_to_channel ("bench", std::move (payload))
                          .timeout (std::chrono::milliseconds (500))
                          .async<payload_t> ();
            if (task.result ())
                return true;
            if (!_readiness_error_reported) {
                std::fprintf (stderr, "framework route readiness: %s\n", task.result ().error ()->what ());
                _readiness_error_reported = true;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
        return false;
    }

    void run (clock_t_::time_point deadline, size_t size, phase_t phase,
              counters_t &counters, latency_sampler_t *latency) override
    {
        if (_command)
            run_tasks<void> (deadline, size, phase, counters, latency);
        else
            run_tasks<payload_t> (deadline, size, phase, counters, latency);
    }

  private:
    // A hosted service obtains the public channel client once the host has built DI.
    class capture_client_t final : public fw::hosted_service_t
    {
      public:
        explicit capture_client_t (fw::route_client_t *&client) : _client (client) {}
        fw::task_t<void> start (fw::service_provider_t &services) override
        {
            _client = &services.get_required<fw::route_client_t> ();
            co_return;
        }
        void stop () noexcept override {}
      private:
        fw::route_client_t *&_client;
    };

    payload_t make_payload (size_t size, phase_t phase)
    {
        payload_t payload;
        payload.mutable_body ()->assign (size, '\xab');
        stamp_payload (payload.mutable_body ()->data (), size, _run_id, phase, _seq++);
        return payload;
    }

    template<typename T>
    fw::task_t<T> submit (payload_t payload)
    {
        if constexpr (std::is_void_v<T>)
            return _client->send_to_channel ("bench", std::move (payload)).async ();
        else
            return _client->request_to_channel ("bench", std::move (payload))
              .timeout (std::chrono::milliseconds (_options.request_timeout_ms))
              .async<payload_t> ();
    }

    template<typename T>
    void run_tasks (clock_t_::time_point deadline, size_t size, phase_t phase,
                    counters_t &counters, latency_sampler_t *latency)
    {
        struct pending_t
        {
            pending_t (fw::task_t<T> task_, uint64_t sent_ns_, uint64_t seq_) :
                task (std::move (task_)), sent_ns (sent_ns_), seq (seq_)
            {
            }

            fw::task_t<T> task;
            uint64_t sent_ns;
            uint64_t seq;
        };
        std::vector<std::optional<pending_t>> pending;
        if (_window > 0)
            pending.reserve (static_cast<size_t> (_window));
        std::vector<size_t> free_slots;
        std::vector<size_t> completed_slots;
        size_t in_flight = 0;
        auto completion_signal = std::make_shared<completion_signal_t> ();
        const auto hard_stop = deadline + std::chrono::milliseconds (_options.drain_bound_ms);
        do {
            close_window (deadline);
            // Unbounded admission submits once per cooperative completion-pump turn.
            while (clock_t_::now () < deadline
                   && (_window <= 0 || in_flight < static_cast<size_t> (_window))) {
                const auto sent = now_ns ();
                auto payload = make_payload (size, phase);
                counters.enter ();
                ++counters.submitted;
                size_t slot = 0;
                if (free_slots.empty ()) {
                    slot = pending.size ();
                    pending.emplace_back (std::nullopt);
                } else {
                    slot = free_slots.back ();
                    free_slots.pop_back ();
                }
                pending[slot].emplace (submit<T> (std::move (payload)), sent, _seq - 1);
                fw::detail::observe_task_completion (
                  pending[slot]->task, [completion_signal, slot] (const auto &) {
                      completion_signal->notify (slot);
                  });
                ++in_flight;
                if (_window <= 0)
                    break;
            }
            completion_signal->take_completed (completed_slots);
            for (const size_t slot : completed_slots) {
                auto &item = *pending[slot];
                const auto &result = item.task.result ();
                bool valid = static_cast<bool> (result);
                if constexpr (!std::is_void_v<T>) {
                    if (valid) {
                        const auto &reply = result.value ();
                        decoded_header_t header;
                        valid = decode_payload (reply.body ().data (), reply.body ().size (), &header)
                          && header.run_id == _run_id && header.phase == phase
                          && header.payload_size == size && reply.body ().size () == size && header.seq == item.seq;
                        if (!valid) ++counters.header_failures;
                    }
                }
                if (valid) {
                    ++counters.completed;
                    if (latency) latency->add_us (static_cast<double> (now_ns () - item.sent_ns) / 1000.0);
                } else {
                    ++counters.errors;
                    if (counters.errors.load () == 1 && !result)
                        std::fprintf (stderr, "framework operation failed: %s\n", result.error ()->what ());
                }
                counters.leave ();
                pending[slot].reset ();
                free_slots.push_back (slot);
                --in_flight;
            }

            const auto now = clock_t_::now ();
            if (now >= hard_stop)
                break;
            const bool can_submit = now < deadline
              && (_window <= 0 || in_flight < static_cast<size_t> (_window));
            if (in_flight != 0 && !can_submit)
                completion_signal->wait_until (now < deadline ? deadline : hard_stop);
        } while (clock_t_::now () < deadline || in_flight != 0);
        close_window (deadline);
    }

    const options_t &_options;
    int _window;
    bool _command;
    stats_http_server_t _host;
    fw::route_client_t *_client = nullptr;
    bool _readiness_error_reported = false;
    uint32_t _run_id = static_cast<uint32_t> (now_ns ());
    uint64_t _seq = 0;
};

int pattern_window (const std::string &pattern)
{
    if (pattern == "request-serial") return 1;
    if (pattern == "request-window") return 100;
    if (pattern == "request-backpressure") return 0;
    if (pattern == "send-saturation") return 8;
    throw std::runtime_error ("unknown pattern: " + pattern);
}

// One source owns phase admission, the two acknowledgement records and measurements.
class source_t
{
  public:
    explicit source_t (options_t options) : _options (std::move (options)),
        _http (std::vector<int>{_options.trigger_port, _options.stats_port},
               [this] (const std::string &method, const std::string &path, const std::string &body) {
                   return handle (method, path, body);
               })
    {
        (void) pattern_window (_options.pattern);
    }

    ~source_t ()
    {
        if (_worker.joinable ()) _worker.join ();
        _http.stop ();
    }

    bool start ()
    {
        if (!_http.start ()) return false;
        // The machine's ephemeral range includes the reserved bench band. Claim
        // both HTTP listeners before any outbound transport can allocate a port.
        if (!wait_ready ("127.0.0.1", _options.trigger_port, 30000)
            || !wait_ready ("127.0.0.1", _options.stats_port, 30000)) return false;
        bool prepared = false;
        _worker = std::thread ([this, &prepared] {
            try {
                const auto window = pattern_window (_options.pattern);
                const bool command = _options.pattern == "send-saturation";
                if (_options.implementation == "grpc-cpp")
                    _driver = std::make_unique<grpc_driver_t> (_options, window, command);
                else if (_options.implementation == "zlink-cpp")
                    _driver = std::make_unique<zlink_raw_driver_t> (_options, window, command);
                else if (_options.implementation == "zlink-framework-cpp")
                    _driver = std::make_unique<framework_driver_t> (_options, window, command);
                else throw std::runtime_error ("unknown implementation: " + _options.implementation);

                const bool ready = _driver->await_ready (_options.readiness_timeout_ms);
                std::lock_guard lock (_gate);
                prepared = ready;
                if (!ready) { _phase = "failed"; _failure = "route readiness timed out"; }
            } catch (const std::exception &error) {
                fail (error.what ());
            }
        });
        // Channel-only Framework hosting installs process signal handlers too;
        // main installs the bench handler after all hosts have started.
        _worker.join ();
        {
            std::lock_guard lock (_gate);
            _ready = prepared;
        }
        return true;
    }

  private:
    bench_http_reply_t handle (const std::string &method, const std::string &path, const std::string &body)
    {
        if (method == "GET" && (path == "/bench/stats" || path == "/ready")) {
            std::lock_guard lock (_gate);
            return {200, json{{"role", "source"}, {"ready", _ready}, {"phase", _phase},
              {"submitted", _counters.submitted.load ()}, {"completed", _counters.completed.load ()},
              {"errors", _counters.errors.load ()}, {"inFlight", _counters.outstanding.load ()},
              {"currentInFlight", _counters.outstanding.load ()}, {"failure", _failure}}.dump ()};
        }
        if (method != "POST" || path != "/bench/start")
            return {404, R"({"error":"not found"})"};
        try {
            const auto request = json::parse (body);
            const std::vector<std::string> fields = {"runId", "cellId", "pattern", "payloadBytes",
              "phase", "durationMs", "requestWindow", "sendConcurrency"};
            if (!request.is_object () || request.size () != fields.size ()
                || !std::all_of (fields.begin (), fields.end (), [&] (const auto &key) { return request.contains (key); })
                || !request.at ("payloadBytes").is_number_integer ()
                || !request.at ("durationMs").is_number_integer ()
                || !request.at ("requestWindow").is_number_integer ()
                || !request.at ("sendConcurrency").is_number_integer ())
                return {400, R"({"error":"trigger requires its eight contract fields"})"};
            const auto run = request.at ("runId").get<std::string> ();
            const auto cell = request.at ("cellId").get<std::string> ();
            const auto phase = request.at ("phase").get<std::string> ();
            const auto duration = request.at ("durationMs").get<int64_t> ();
            if (run.empty () || cell.empty () || (phase != "warmup" && phase != "active")
                || request.at ("pattern") != _options.pattern
                || request.at ("payloadBytes") != _options.payload_size
                || duration <= 0 || request.at ("requestWindow") != 100
                || request.at ("sendConcurrency") != 8)
                return {400, R"({"error":"invalid trigger conditions"})"};
            std::lock_guard lock (_gate);
            const auto key = json::array ({run, cell, phase}).dump ();
            if (auto found = _acks.find (key); found != _acks.end ())
                return {200, found->second};
            if (!_ready || _phase != "idle" || (phase == "active" && !_warmed)
                || (phase == "warmup" && _warmed) || (!_cell_id.empty () && (_cell_id != cell || _run_id != run)))
                return {409, R"({"error":"source is not ready for this phase"})"};
            // idle is published as the worker's final action; joining it cannot wait on _gate.
            if (_worker.joinable ()) _worker.join ();
            _run_id = run;
            _cell_id = cell;
            _phase = phase;
            _counters.reset ();
            _counters.outstanding = 0;
            const auto ack = json{{"accepted", true}, {"runId", run}, {"cellId", cell},
              {"phase", phase}, {"startedAt", now_ns ()}}.dump ();
            _acks.emplace (key, ack);
            const auto received_at = std::chrono::duration_cast<std::chrono::milliseconds> (
              std::chrono::system_clock::now ().time_since_epoch ()).count ();
            json trigger = {{"runId", run}, {"cellId", cell}, {"pattern", _options.pattern},
              {"payloadBytes", _options.payload_size}, {"durationMs", duration},
              {"warmup", static_cast<int64_t> (_options.warmup_seconds * 1000)},
              {"endpoint", "http://127.0.0.1:" + std::to_string (_options.trigger_port) + "/bench/start"},
              {"receivedAtUnixMs", received_at}};
            _worker = std::thread ([this, phase, duration, trigger] {
                try {
                    run_phase (phase, duration, trigger);
                    // Errors and abandoned operations are cell results (spec 5.2: recorded,
                    // excluded from the throughput judgement), not a phase failure. Only an
                    // exception or a readiness timeout fails the phase, as in the .NET source.
                    std::lock_guard lock (_gate);
                    if (phase == "warmup") _warmed = true;
                    _phase = "idle";
                } catch (const std::exception &error) { fail (error.what ()); }
            });
            return {200, ack};
        } catch (const std::exception &error) {
            return {400, json{{"error", error.what ()}}.dump ()};
        }
    }

    void fail (const std::string &message)
    {
        std::fprintf (stderr, "source failed: %s\n", message.c_str ());
        std::lock_guard lock (_gate);
        _phase = "failed";
        _failure = message;
    }

    void run_phase (const std::string &phase, int64_t duration, const json &trigger)
    {
        const bool active = phase == "active";
        if (!active) {
            _warmup_throughput.clear ();
            for (int segment = 0; segment < _options.warmup_segments; ++segment) {
                const auto before = _counters.completed.load ();
                const auto segment_start = clock_t_::now ();
                const auto segment_deadline = segment_start + std::chrono::microseconds (
                  duration * 1000 / _options.warmup_segments);
                _driver->run (segment_deadline, _options.payload_size, phase_warmup, _counters, nullptr);
                const auto elapsed = std::chrono::duration<double> (clock_t_::now () - segment_start).count ();
                _warmup_throughput.push_back ((_counters.completed.load () - before) / elapsed);
                if (_counters.errors != 0 || _counters.outstanding != 0) return;
            }
            return;
        }
        latency_sampler_t latency (_options.latency_sample_limit);
        cell_t cell;
        cell.implementation = _options.implementation;
        cell.pattern = _options.pattern;
        cell.payload_size = _options.payload_size;
        cell.request_window = pattern_window (_options.pattern);
        cell.logical_cores_value = logical_cores ();
        const auto target_before = fetch_stats ("127.0.0.1", _options.target_stats_port);
        if (!target_before) throw std::runtime_error ("target stats unavailable before phase");
        const auto cpu_before = process_cpu_seconds_self ();
        const auto submit_before = thread_cpu_seconds_self ();
        const auto start = clock_t_::now ();
        const auto deadline = start + std::chrono::milliseconds (duration);
        long long completed_at_close = 0;
        double elapsed = 0;
        _driver->boundary = [&] {
            // Snapshot before any HTTP work or terminal completion drain.
            elapsed = std::chrono::duration<double> (clock_t_::now () - start).count ();
            completed_at_close = _counters.completed;
            cell.client_cores = (process_cpu_seconds_self () - cpu_before) / elapsed;
            cell.submit_thread_cores = (thread_cpu_seconds_self () - submit_before) / elapsed;
            cell.non_submit_cores = std::max (0.0, cell.client_cores - cell.submit_thread_cores);
            cell.client_cpu_percent = cell.client_cores / logical_cores () * 100;
            cell.client_memory_mb = rss_mb ();
            cell.client_threads = thread_count_self ();
            cell.latency_mean_ms = latency.mean_us () / 1000;
            cell.latency_p95_ms = latency.percentile (0.95) / 1000;
            cell.latency_p99_ms = latency.percentile (0.99) / 1000;
            const auto boundary = fetch_stats ("127.0.0.1", _options.target_stats_port);
            if (!boundary) throw std::runtime_error ("target boundary stats unavailable");
            cell.server_received_at_close = boundary->active_messages;
            cell.server_cpu_percent = (boundary->cpu_seconds - target_before->cpu_seconds) / elapsed / logical_cores () * 100;
            cell.server_memory_mb = boundary->rss_mb;
            if (_options.pattern == "send-saturation") {
                cell.latency_mean_ms = boundary->mean_us / 1000;
                cell.latency_p95_ms = boundary->p95_us / 1000;
                cell.latency_p99_ms = boundary->p99_us / 1000;
            }
        };
        _driver->run (deadline, _options.payload_size, active ? phase_active : phase_warmup,
                      _counters, active ? &latency : nullptr);
        _driver->close_window (deadline);
        cell.completed = _counters.completed;
        cell.submitted = _counters.submitted;
        cell.errors = _counters.errors;
        cell.abandoned = _counters.outstanding;
        cell.peak_in_flight = _counters.peak_in_flight;
        cell.header_validation_failures = _counters.header_failures;
        cell.warmup_segment_throughput = _warmup_throughput;
        cell.throughput_per_second = static_cast<double> (_options.pattern == "send-saturation"
          ? cell.server_received_at_close.value () : completed_at_close) / elapsed;
        cell.bandwidth_mb_s = cell.throughput_per_second * cell.payload_size / 1e6;
        cell.drain_ms = std::chrono::duration<double, std::milli> (clock_t_::now () - deadline).count ();
        cell.drain_bound_hit = cell.abandoned != 0;
        const std::map<std::string, std::string> metadata = {
          {"model", "server-driven"}, {"warmupSeconds", std::to_string (_options.warmup_seconds)},
          {"warmupSegments", std::to_string (_options.warmup_segments)}, {"warmupUnit", "milliseconds"}, {"grpcVersion", grpc::Version ()},
          {"grpcServer", "synchronous ServerBuilder defaults; insecure loopback; no tuning"},
          {"compiler", __VERSION__}, {"buildType", "Release"},
          {"frameworkVersion", BENCH_FRAMEWORK_VERSION}, {"bindingVersion", BENCH_BINDING_VERSION}, {"coreVersion", BENCH_CORE_VERSION},
          {"protobufVersion", BENCH_PROTOBUF_VERSION}};
        const auto temporary = _options.output_file + ".source";
        write_cells_json (temporary, {cell}, metadata);
        std::ifstream input (temporary);
        auto record = json::parse (input);
        input.close ();
        auto &value = record["cells"][0];
        value["role"] = "source";
        value["trigger"] = trigger;
        value["streams"] = {{"count", _options.pattern == "send-saturation" ? 8 : 1},
          {"inFlightPerStream", _options.pattern == "request-backpressure" ? json (nullptr)
             : json (_options.pattern == "request-window" ? 100 : 1)},
          {"implementation", _options.implementation == "grpc-cpp" ? "one application thread; async unary CompletionQueue; one stub per logical stream"
             : _options.implementation == "zlink-cpp" ? "one application thread; coroutine slots and binding completion poller"
             : "one application thread; Framework task completion notifications"}};
        value["completed_at_close"] = completed_at_close;
        value["active_elapsed_ms"] = elapsed * 1000;
        std::ofstream output (temporary, std::ios::trunc);
        output << record.dump (2) << '\n';
        output.close ();
        if (!output) throw std::runtime_error ("cannot write source result");
        std::filesystem::rename (temporary, _options.output_file);
        print_result_lines (stdout, cell);
        std::fflush (stdout);
    }

    options_t _options;
    stats_http_server_t _http;
    std::unique_ptr<driver_t> _driver;
    std::mutex _gate;
    bool _ready = false;
    bool _warmed = false;
    std::string _phase = "idle", _run_id, _cell_id, _failure;
    std::map<std::string, std::string> _acks;
    counters_t _counters;
    std::vector<double> _warmup_throughput;
    std::thread _worker;
};

std::atomic<bool> stop_source {false};
void source_signal (int) { stop_source = true; }
} // namespace

int main (int argc, char **argv)
{
    try {
        options_t options;
        options.implementation = arg_value (argc, argv, "--implementation", "grpc-cpp");
        options.pattern = arg_value (argc, argv, "--pattern", "request-serial");
        options.payload_size = std::stoul (arg_value (argc, argv, "--payload-size", "1024"));
        if (options.payload_size != 1024 && options.payload_size != 4096)
            throw std::runtime_error ("payload must be 1024 or 4096");
        const auto endpoint = arg_value (argc, argv, "--endpoint", "127.0.0.1:5282");
        options.grpc_endpoint = options.raw_request_endpoint = options.framework_endpoint = endpoint;
        options.raw_command_endpoint = arg_value (argc, argv, "--command-endpoint", "tcp://127.0.0.1:5288");
        options.trigger_port = std::stoi (arg_value (argc, argv, "--trigger-port", "5280"));
        options.stats_port = std::stoi (arg_value (argc, argv, "--stats-port", "5281"));
        options.target_stats_port = std::stoi (arg_value (argc, argv, "--target-stats-port", "5283"));
        options.warmup_seconds = std::stod (arg_value (argc, argv, "--warmup-seconds", "5"));
        options.warmup_segments = std::stoi (arg_value (argc, argv, "--warmup-segments", "10"));
        options.request_timeout_ms = std::stoi (arg_value (argc, argv, "--request-timeout-ms", "30000"));
        options.drain_bound_ms = std::stoi (arg_value (argc, argv, "--drain-bound-ms", "30000"));
        if (options.warmup_seconds <= 0 || options.warmup_segments < 1
            || options.request_timeout_ms <= 0 || options.drain_bound_ms != 30000)
            throw std::runtime_error ("invalid warmup, timeout or settle bound");
        options.output_file = arg_value (argc, argv, "--output-file", "cells.json");
        source_t source (std::move (options));
        if (!source.start ()) return 2;
        std::signal (SIGINT, source_signal);
        std::signal (SIGTERM, source_signal);
        while (!stop_source) std::this_thread::sleep_for (std::chrono::milliseconds (100));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf (stderr, "source failed: %s\n", error.what ());
        return 2;
    }
}
