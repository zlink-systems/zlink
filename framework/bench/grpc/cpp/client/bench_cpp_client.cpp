/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
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
#include <latch>
#include <pthread.h>
#include <cxxabi.h>

#include "bench.grpc.pb.h"
#include "bench.pb.h"

#include <grpcpp/grpcpp.h>

#include <zlink.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <type_traits>
#include <vector>


using namespace zlink_cpp_bench;
using clock_t_ = std::chrono::steady_clock;

namespace
{


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
    double target_rate = 0;
    int submit_concurrency = 8;
    std::string output_file;
};

class submission_pacer_t
{
  public:
    explicit submission_pacer_t (double rate) : _rate (rate) {}

    // Both drivers call this only when target_rate > 0; rate 0 keeps the unpaced paths.
    size_t wait_for_due_slots (clock_t_::time_point deadline)
    {
        assert (_rate > 0);
        auto interval = std::chrono::duration_cast<clock_t_::duration> (
          std::chrono::duration<double> (1.0 / _rate));
        if (interval <= clock_t_::duration::zero ())
            interval = clock_t_::duration (1);
        const auto now = clock_t_::now ();
        const auto scheduled = std::max (now, _next);
        if (scheduled >= deadline)
            return 0;
        std::this_thread::sleep_until (scheduled);
        const auto woke = clock_t_::now ();
        if (woke >= deadline)
            return 0;
        const size_t due = 1 + static_cast<size_t> (
          std::chrono::duration_cast<clock_t_::duration> (woke - scheduled) / interval);
        _next = scheduled + interval * due;
        return due;
    }

  private:
    double _rate;
    clock_t_::time_point _next{};
};

// ---------------------------------------------------------------------------
// per-cell measurement state
// ---------------------------------------------------------------------------

struct error_record_t
{
    std::string type;
    std::string message;
    long long count = 0;
};

struct counters_t
{
    static constexpr size_t error_kind_limit = 8;
    static constexpr size_t error_message_limit = 200;
    std::atomic<long long> completed{0};
    std::atomic<long long> errors{0};
    std::atomic<long long> submitted{0};
    std::atomic<long long> header_failures{0};
    std::atomic<long long> outstanding{0};
    std::atomic<long long> peak_in_flight{0};
    std::vector<error_record_t> error_summary;
    long long other_errors = 0;

    void enter ()
    {
        const long long now = outstanding.fetch_add (1, std::memory_order_acq_rel) + 1;
        long long peak = peak_in_flight.load (std::memory_order_relaxed);
        while (now > peak
               && !peak_in_flight.compare_exchange_weak (peak, now, std::memory_order_relaxed))
            ;
    }

    void leave () { outstanding.fetch_sub (1, std::memory_order_acq_rel); }

    void record_error (std::string type, std::string message, long long count = 1)
    {
        errors.fetch_add (count, std::memory_order_relaxed);
        std::replace (message.begin (), message.end (), '\r', ' ');
        std::replace (message.begin (), message.end (), '\n', ' ');
        if (message.size () > error_message_limit)
            message.resize (error_message_limit);
        const auto found = std::find_if (error_summary.begin (), error_summary.end (),
                                         [&] (const error_record_t &entry) {
                                             return entry.type == type && entry.message == message;
                                         });
        if (found != error_summary.end ())
            found->count += count;
        else if (error_summary.size () < error_kind_limit)
            error_summary.push_back ({std::move (type), std::move (message), count});
        else
            other_errors += count;
    }

    void record_exception (const std::exception &error)
    {
        int status = 0;
        std::unique_ptr<char, decltype (&std::free)> demangled (
          abi::__cxa_demangle (typeid (error).name (), nullptr, nullptr, &status), &std::free);
        record_error (status == 0 && demangled ? demangled.get () : typeid (error).name (),
                      error.what ());
    }

    nlohmann::json error_summary_json () const
    {
        auto value = nlohmann::json::array ();
        for (const auto &error : error_summary)
            value.push_back ({{"type", error.type}, {"message", error.message}, {"count", error.count}});
        return value;
    }

    void reset ()
    {
        completed.store (0);
        errors.store (0);
        submitted.store (0);
        header_failures.store (0);
        peak_in_flight.store (0);
        error_summary.clear ();
        other_errors = 0;
        // `outstanding` is deliberately NOT reset: requests issued during warmup
        // that are still open are genuinely still open when the active window
        // starts, and zeroing the counter here would hide them.
    }
};

namespace fw = zlink::framework;
using payload_t = zlink::framework::bench::withgrpc::BenchPayload;
using json = nlohmann::json;

// A completion resource writes only the originating stream's record. The submit
// thread reads that record to await admission/completion; no callback is rescheduled.
struct stream_t
{
    std::mutex mutex;
    std::condition_variable changed;
    counters_t counters;
    latency_sampler_t latency;
    clock_t_::time_point deadline;
    uint32_t run_id;
    phase_t phase;
    size_t size;
    bool admitted = true;
    bool stopped = false;
    bool closed = false;
    long long active_completed = 0;
    double submit_cpu = 0;

    stream_t (size_t sample_limit, clock_t_::time_point end, uint32_t run,
              phase_t phase_, size_t size_) :
        latency (sample_limit), deadline (end), run_id (run), phase (phase_), size (size_) {}

    void admit (bool ok = true)
    {
        std::lock_guard lock (mutex);
        admitted = true;
        stopped = !ok;
        changed.notify_one ();
    }

    void finish (uint64_t sent, const payload_t *reply, uint64_t seq,
                 const std::exception *error = nullptr)
    {
        std::lock_guard lock (mutex);
        if (closed)
            return;
        const auto now = clock_t_::now ();
        bool valid = !error;
        if (reply && valid) {
            decoded_header_t header{};
            valid = decode_payload (reply->body ().data (), reply->body ().size (), &header)
                    && header.run_id == run_id && header.phase == phase
                    && header.payload_size == size && reply->body ().size () == size
                    && header.seq == seq;
            if (!valid)
                ++counters.header_failures;
            else if (sent == 0)
                sent = header.sent_ns;
        }
        if (error)
            counters.record_exception (*error);
        else if (!valid)
            counters.record_error ("ValidationError", "reply header mismatch");
        else {
            ++counters.completed;
            if (now < deadline) {
                ++active_completed;
                if (sent)
                    latency.add_us (static_cast<double> (now_ns () - sent) / 1000.0);
            }
        }
        counters.leave ();
        changed.notify_one ();
    }
};

class driver_t
{
  public:
    driver_t (const options_t &options, int window, bool command) :
        _options (options), _window (window), _command (command) {}
    virtual ~driver_t () = default;
    int request_window () const { return _window; }
    int stream_count () const
    {
        return _options.pattern == "request-serial" || _options.pattern == "request-window"
          ? 1 : _options.submit_concurrency;
    }
    json streams_metadata () const
    {
        return {{"count", stream_count ()},
                {"inFlightPerStream", _window <= 0 ? json (nullptr) : json (_window)},
                {"implementation", "one dedicated submit thread per stream; native completion resources"}};
    }
    std::function<void ()> boundary;
    long long completed_at_close = 0;
    double submit_cpu_seconds = 0;
    std::vector<double> warmup_segment_throughput;
    void close_window (clock_t_::time_point deadline)
    {
        if (boundary && clock_t_::now () >= deadline) {
            auto sample = std::move (boundary);
            sample ();
        }
    }
    virtual bool await_ready (int timeout_ms) = 0;

    struct progress_t { long long submitted = 0, completed = 0, errors = 0, outstanding = 0; };
    progress_t progress (const counters_t &terminal)
    {
        std::lock_guard lock (_streams_mutex);
        if (_streams.empty ())
            return {terminal.submitted.load (), terminal.completed.load (), terminal.errors.load (), terminal.outstanding.load ()};
        progress_t result;
        for (const auto &stream : _streams) {
            std::lock_guard stream_lock (stream->mutex);
            result.submitted += stream->counters.submitted;
            result.completed += stream->counters.completed;
            result.errors += stream->counters.errors;
            result.outstanding += stream->counters.outstanding;
        }
        return result;
    }

    void run (clock_t_::time_point deadline, size_t size, phase_t phase,
              counters_t &counters, latency_sampler_t *latency)
    {
        const int count = stream_count ();
        prepare_streams (count);
        std::vector<std::shared_ptr<stream_t>> streams;
        std::vector<std::thread> threads;
        std::latch submitted_done (count);
        std::latch boundary_done (1);
        for (int index = 0; index < count; ++index) {
            const size_t sample_limit = latency
              ? _options.latency_sample_limit / count
                  + (static_cast<size_t> (index) < _options.latency_sample_limit % count)
              : 0;
            auto stream = std::make_shared<stream_t> (
              sample_limit, deadline, _run_id, phase, size);
            streams.push_back (stream);
            threads.emplace_back ([&, stream, index] {
                const auto name = "bench-submit-" + std::to_string (index);
                pthread_setname_np (pthread_self (), name.c_str ());
                const double cpu_start = thread_cpu_seconds_self ();
                submission_pacer_t pacer (_options.target_rate / count);
                size_t due = 0;

                while (clock_t_::now () < deadline) {
                    {
                        std::unique_lock lock (stream->mutex);
                        stream->changed.wait_until (lock, deadline, [&] {
                            return stream->stopped || (_command
                              ? can_submit_framework_task<void> (clock_t_::now (), deadline,
                                  stream->counters.outstanding, _window, [] { return true; })
                              : can_submit_framework_task<payload_t> (clock_t_::now (), deadline,
                                  stream->counters.outstanding, _window, [&] { return stream->admitted; }));
                        });
                        if (stream->stopped || clock_t_::now () >= deadline)
                            break;
                    }
                    if (_options.target_rate > 0) {
                        if (due == 0)
                            due = pacer.wait_for_due_slots (deadline);
                        if (due == 0)
                            break;
                        --due;
                    }
                    {
                        std::lock_guard lock (stream->mutex);
                        stream->admitted = false;
                        stream->counters.enter ();
                        ++stream->counters.submitted;
                    }
                    try {
                        submit (stream, index, _sequence.fetch_add (1, std::memory_order_relaxed));
                    } catch (const std::exception &error) {
                        stream->finish (0, nullptr, 0, &error);
                        stream->admit (false);
                    }
                }
                stream->submit_cpu = thread_cpu_seconds_self () - cpu_start;
                submitted_done.count_down ();
                boundary_done.wait ();
                std::unique_lock lock (stream->mutex);
                stream->changed.wait_until (
                  lock, deadline + std::chrono::milliseconds (_options.drain_bound_ms),
                  [&] { return stream->counters.outstanding == 0; });
                stream->closed = true;
            });
        }
        { std::lock_guard lock (_streams_mutex); _streams = streams; }
        warmup_segment_throughput.clear ();
        const auto sample_start = clock_t_::now ();
        auto previous_time = sample_start;
        long long previous_completed = 0;
        const int segments = phase == phase_warmup ? _options.warmup_segments : 1;
        for (int segment = 1; segment <= segments; ++segment) {
            std::this_thread::sleep_until (
              sample_start + (deadline - sample_start) * segment / segments);
            if (phase == phase_warmup) {
                long long completed = 0;
                for (const auto &stream : streams) {
                    std::lock_guard lock (stream->mutex);
                    completed += stream->active_completed;
                }
                const auto now = clock_t_::now ();
                const auto elapsed = std::chrono::duration<double> (now - previous_time).count ();
                warmup_segment_throughput.push_back ((completed - previous_completed) / elapsed);
                previous_completed = completed;
                previous_time = now;
            }
        }
        submitted_done.wait ();
        completed_at_close = 0;
        submit_cpu_seconds = 0;
        for (const auto &stream : streams) {
            std::lock_guard lock (stream->mutex);
            completed_at_close += stream->active_completed;
            submit_cpu_seconds += stream->submit_cpu;
        }
        std::exception_ptr boundary_error;
        try { close_window (deadline); }
        catch (...) { boundary_error = std::current_exception (); }
        boundary_done.count_down ();
        for (auto &thread : threads)
            thread.join ();

        // The only aggregate merge: every stream's terminal record is frozen.
        for (const auto &stream : streams) {
            std::lock_guard lock (stream->mutex);
            auto &from = stream->counters;
            counters.completed += from.completed.load ();
            counters.submitted += from.submitted.load ();
            counters.outstanding += from.outstanding.load ();
            counters.header_failures += from.header_failures.load ();
            counters.peak_in_flight += from.peak_in_flight.load ();
            for (const auto &error : from.error_summary)
                counters.record_error (error.type, error.message, error.count);
            counters.errors += from.other_errors;
            counters.other_errors += from.other_errors;
            if (latency)
                latency->merge (stream->latency);
        }
        { std::lock_guard lock (_streams_mutex); _streams.clear (); }
        if (boundary_error)
            std::rethrow_exception (boundary_error);
    }

  protected:
    payload_t make_payload (size_t size, phase_t phase, uint64_t sequence)
    {
        payload_t payload;
        payload.mutable_body ()->assign (size, '\xab');
        stamp_payload (payload.mutable_body ()->data (), size, _run_id, phase, sequence);
        return payload;
    }
    virtual void prepare_streams (int) {}
    virtual void submit (std::shared_ptr<stream_t> stream, int index, uint64_t seq) = 0;
    const options_t &_options;
    int _window;
    bool _command;
    uint32_t _run_id = static_cast<uint32_t> (now_ns ());
    std::atomic<uint64_t> _sequence{1};
    std::mutex _streams_mutex;
    std::vector<std::shared_ptr<stream_t>> _streams;
};

class grpc_driver_t final : public driver_t
{
  public:
    grpc_driver_t (const options_t &options, int window, bool command) :
        driver_t (options, window, command),
        _channel (grpc::CreateChannel (options.grpc_endpoint, grpc::InsecureChannelCredentials ())),
        _completion_thread ([this] {
            pthread_setname_np (pthread_self (), "bench-grpc-cq");
            void *tag = nullptr;
            bool ok = false;
            while (_cq.Next (&tag, &ok)) {
                std::unique_ptr<call_base_t> call (static_cast<call_base_t *> (tag));
                call->finish (ok);
            }
        }) {}
    ~grpc_driver_t () override
    {
        _cq.Shutdown ();
        _completion_thread.join ();
    }
    bool await_ready (int timeout_ms) override
    {
        auto stub = zlink::framework::bench::withgrpc::BenchService::NewStub (_channel);
        const auto deadline = clock_t_::now () + std::chrono::milliseconds (timeout_ms);
        while (clock_t_::now () < deadline) {
            grpc::ClientContext context;
            context.set_deadline (std::chrono::system_clock::now () + std::chrono::seconds (2));
            auto request = make_payload (1024, phase_warmup, 0);
            payload_t reply;
            if (stub->Echo (&context, request, &reply).ok () && request.body () == reply.body ())
                return true;
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
        return false;
    }

  private:
    void prepare_streams (int count) override
    {
        _stubs.clear ();
        for (int i = 0; i < count; ++i)
            _stubs.push_back (zlink::framework::bench::withgrpc::BenchService::NewStub (_channel));
    }
    struct call_base_t
    {
        virtual ~call_base_t () = default;
        virtual void finish (bool ok) = 0;
    };
    template <typename Reply> struct call_t final : call_base_t
    {
        grpc::ClientContext context;
        Reply reply;
        grpc::Status status;
        std::unique_ptr<grpc::ClientAsyncResponseReader<Reply>> reader;
        std::shared_ptr<stream_t> stream;
        uint64_t seq = 0, sent = 0;

        void finish (bool ok) override
        {
            if (!ok || !status.ok ()) {
                const std::runtime_error error (ok
                  ? "gRPC code " + std::to_string (status.error_code ()) + ": " + status.error_message ()
                  : "gRPC completion queue event was not ok");
                stream->finish (0, nullptr, seq, &error);
            } else {
                const payload_t *body = nullptr;
                if constexpr (std::is_same_v<Reply, payload_t>)
                    body = &reply;
                stream->finish (body ? sent : 0, body, seq);
            }
        }
    };
    template <typename Reply>
    void start (std::shared_ptr<stream_t> stream, int index, uint64_t seq)
    {
        auto call = std::make_unique<call_t<Reply>> ();
        call->stream = stream;
        call->seq = seq;
        call->context.set_deadline (std::chrono::system_clock::now ()
                                   + std::chrono::milliseconds (_options.request_timeout_ms));
        auto request = make_payload (stream->size, stream->phase, seq);
        call->sent = now_ns ();
        if constexpr (std::is_same_v<Reply, payload_t>)
            call->reader = _stubs[index]->PrepareAsyncEcho (&call->context, request, &_cq);
        else
            call->reader = _stubs[index]->PrepareAsyncCommand (&call->context, request, &_cq);
        call->reader->StartCall ();
        auto *published = call.release ();
        published->reader->Finish (&published->reply, &published->status,
                                   static_cast<call_base_t *> (published));
        stream->admit ();
        // Preserve the uncapped driver's cooperative yield. CQ callbacks stay
        // on its completion resource; no stream waits for a reply here.
        if (_window <= 0)
            std::this_thread::yield ();
    }
    void submit (std::shared_ptr<stream_t> stream, int index, uint64_t seq) override
    {
        if (_command)
            start<google::protobuf::Empty> (stream, index, seq);
        else
            start<payload_t> (stream, index, seq);
    }
    std::shared_ptr<grpc::Channel> _channel;
    std::vector<std::unique_ptr<zlink::framework::bench::withgrpc::BenchService::Stub>> _stubs;
    grpc::CompletionQueue _cq;
    std::thread _completion_thread;
};

class zlink_raw_driver_t final : public driver_t
{
  public:
    zlink_raw_driver_t (const options_t &options, int window, bool command) :
        driver_t (options, window, command), _socket (_context),
        _target (zlink::routing_id_t::from (command ? options.raw_command_rid : options.raw_request_rid))
    {
        _socket.set_routing_id (zlink::routing_id_t::from (
          "bench-source-" + std::to_string (::getpid ())));
        _socket.connect (command ? options.raw_command_endpoint : options.raw_request_endpoint);
        _poller.add (_socket, zlink::poll_event_flag_t::pollcompletion, 1);
        _completion_thread = std::thread ([this] {
            pthread_setname_np (pthread_self (), "bench-complete");
            zlink::poll_event_t event{};
            while (!_stop)
                _poller.wait (&event, 1, std::chrono::milliseconds (50));
        });
    }
    ~zlink_raw_driver_t () override
    {
        _stop = true;
        _completion_thread.join ();
    }
    bool await_ready (int timeout_ms) override
    {
        const auto deadline = clock_t_::now () + std::chrono::milliseconds (timeout_ms);
        while (clock_t_::now () < deadline) {
            auto stream = std::make_shared<stream_t> (0, deadline, _run_id, phase_warmup, 1024);
            stream->counters.enter ();
            ++stream->counters.submitted;
            try { submit (stream, 0, 0); }
            catch (const std::exception &) {
                std::this_thread::sleep_for (std::chrono::milliseconds (50));
                continue;
            }
            std::unique_lock lock (stream->mutex);
            stream->changed.wait_until (lock, deadline, [&] { return stream->counters.outstanding == 0; });
            if (stream->counters.completed > 0)
                return true;
        }
        return false;
    }

  private:
    void submit (std::shared_ptr<stream_t> stream, int, uint64_t seq) override
    {
        const char *envelope = request_envelope ();
        auto header = zlink::message_t::from (std::as_bytes (
          std::span<const char> (envelope, std::strlen (envelope))));
        const auto encoded = encode_bench_payload (make_payload (stream->size, stream->phase, seq));
        auto body = zlink::message_t::from (std::as_bytes (
          std::span<const char> (encoded.data (), encoded.size ())));
        if (_command) {
            auto submission = _socket.send (_target).message (header).message (body).async ();
            observe_binding (std::move (submission.admitted),
              [stream, seq] { stream->finish (0, nullptr, seq); stream->admit (); },
              [stream, seq] (const auto &error) { stream->finish (0, nullptr, seq, &error); stream->admit (false); });
        } else {
            auto submission = _socket.request (_target).message (header).message (body)
              .timeout (std::chrono::milliseconds (_options.request_timeout_ms)).async ();
            observe_binding (std::move (submission.reply),
              [stream, seq] (const auto &parts) {
                  payload_t reply;
                  if (parts.empty () || !reply.ParseFromArray (parts.back ().data (), static_cast<int> (parts.back ().size ()))) {
                      const std::runtime_error error ("invalid raw reply protobuf");
                      stream->finish (0, nullptr, seq, &error);
                  } else
                      stream->finish (0, &reply, seq);
              },
              [stream, seq] (const auto &error) { stream->finish (0, nullptr, seq, &error); });
            observe_binding (std::move (submission.admitted),
              [stream] { stream->admit (); },
              [stream] (const auto &) { stream->admit (false); });
        }
    }
    zlink::context_t _context;
    zlink::router_socket_t _socket;
    zlink::routing_id_t _target;
    zlink::poller_t _poller;
    std::atomic<bool> _stop{false};
    std::thread _completion_thread;
};

class framework_driver_t final : public driver_t
{
  public:
    framework_driver_t (const options_t &options, int window, bool command) :
        driver_t (options, window, command), _host (std::vector<int>{}, bench_http_callback_t{})
    {
        auto &app = _host.app ();
        auto &config = app.add_zlink_framework ();
        config.codecs ().use (zlink::framework_codecs::protobuf ());
        auto mesh = config.add_route_mesh ("bench");
        mesh.set_object_role (fw::object_role_t::none).listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from ("bench-source"));
        mesh.peer_connections ().connect (_target_node, options.framework_endpoint);
        app.add_hosted_service (std::make_unique<capture_client_t> (_client));
    }
    bool await_ready (int timeout_ms) override
    {
        if (!_host.start ())
            return false;
        const auto deadline = clock_t_::now () + std::chrono::milliseconds (timeout_ms);
        while (clock_t_::now () < deadline) {
            auto task = _client->request_to_node ("bench", _target_node, make_payload (1024, phase_warmup, 0))
              .timeout (std::chrono::milliseconds (500)).async<payload_t> ();
            if (task.result ())
                return true;
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
        return false;
    }

  private:
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
    void submit (std::shared_ptr<stream_t> stream, int, uint64_t seq) override
    {
        const auto sent = _command ? 0 : now_ns ();
        auto payload = make_payload (stream->size, stream->phase, seq);
        if (_command) {
            auto task = _client->send_to_node ("bench", _target_node, std::move (payload)).async ();
            observe_framework (task, [stream, seq] (const auto &result) {
                stream->finish (0, nullptr, seq, result ? nullptr : result.error ());
                stream->admit (static_cast<bool> (result));
            });
        } else {
            auto task = _client->request_to_node ("bench", _target_node, std::move (payload))
              .timeout (std::chrono::milliseconds (_options.request_timeout_ms)).async<payload_t> ();
            observe_framework (task, [stream, sent, seq] (const auto &result) {
                stream->finish (sent, result ? &result.value () : nullptr, seq,
                                result ? nullptr : result.error ());
            });
            auto admission = task.admitted ();
            observe_framework (admission, [stream] (const auto &result) {
                stream->admit (static_cast<bool> (result));
            });
        }
    }
    stats_http_server_t _host;
    zlink::routing_id_t _target_node = zlink::routing_id_t::from ("bench-server");
    fw::route_client_t *_client = nullptr;
};

int pattern_window (const std::string &pattern)
{
    if (pattern == "request-serial")
        return 1;
    if (pattern == "request-window")
        return 100;
    if (pattern == "request-backpressure")
        return unlimited_request_window;
    if (pattern == "send-saturation")
        return 1;
    throw std::runtime_error ("unknown pattern: " + pattern);
}

// One source owns phase admission, the two acknowledgement records and measurements.
class source_t
{
  public:
    explicit source_t (options_t options) :
        _options (std::move (options)),
        _http (std::vector<int>{_options.trigger_port, _options.stats_port},
               [this] (const std::string &method,
                       const std::string &path,
                       const std::string &body) { return handle (method, path, body); })
    {
        (void) pattern_window (_options.pattern);
    }

    ~source_t ()
    {
        if (_worker.joinable ())
            _worker.join ();
        _http.stop ();
    }

    bool start ()
    {
        if (!_http.start ())
            return false;
        // The machine's ephemeral range includes the reserved bench band. Claim
        // both HTTP listeners before any outbound transport can allocate a port.
        if (!wait_ready ("127.0.0.1", _options.trigger_port, 30000)
            || !wait_ready ("127.0.0.1", _options.stats_port, 30000))
            return false;
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
                else
                    throw std::runtime_error ("unknown implementation: " + _options.implementation);

                const bool ready = _driver->await_ready (_options.readiness_timeout_ms);
                std::lock_guard lock (_gate);
                prepared = ready;
                if (!ready) {
                    _phase = "failed";
                    _failure = "route readiness timed out";
                }
            }
            catch (const std::exception &error) {
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
    bench_http_reply_t
    handle (const std::string &method, const std::string &path, const std::string &body)
    {
        if (method == "GET" && (path == "/bench/stats" || path == "/ready")) {
            std::lock_guard lock (_gate);
            const auto progress = _ready ? _driver->progress (_counters) : driver_t::progress_t{};
            return {200, json{{"role", "source"},
                              {"ready", _ready},
                              {"phase", _phase},
                              {"submitted", progress.submitted},
                              {"completed", progress.completed},
                              {"errors", progress.errors},
                              {"inFlight", progress.outstanding},
                              {"currentInFlight", progress.outstanding},
                              {"failure", _failure},
                              {"submitConcurrency", _options.submit_concurrency},
                              {"streams", _ready ? _driver->streams_metadata () : json (nullptr)},
                              {"client_error_summary", _phase == "idle" || _phase == "failed"
                                  ? _counters.error_summary_json () : json (nullptr)},
                              {"client_error_other_count", _phase == "idle" || _phase == "failed"
                                  ? json (_counters.other_errors) : json (nullptr)}}
                           .dump ()};
        }
        if (method != "POST" || path != "/bench/start")
            return {404, R"({"error":"not found"})"};
        try {
            const auto request = json::parse (body);
            const std::vector<std::string> fields = {
              "runId", "cellId",     "pattern",       "payloadBytes",
              "phase", "durationMs", "requestWindow", "submitConcurrency"};
            if (!request.is_object () || request.size () != fields.size ()
                || !std::all_of (fields.begin (), fields.end (),
                                 [&] (const auto &key) { return request.contains (key); })
                || !request.at ("payloadBytes").is_number_integer ()
                || !request.at ("durationMs").is_number_integer ()
                || !request.at ("requestWindow").is_number_integer ()
                || !request.at ("submitConcurrency").is_number_integer ())
                return {400, R"({"error":"trigger requires its eight contract fields"})"};
            const auto run = request.at ("runId").get<std::string> ();
            const auto cell = request.at ("cellId").get<std::string> ();
            const auto phase = request.at ("phase").get<std::string> ();
            const auto duration = request.at ("durationMs").get<int64_t> ();
            if (run.empty () || cell.empty () || (phase != "warmup" && phase != "active")
                || request.at ("pattern") != _options.pattern
                || request.at ("payloadBytes") != _options.payload_size || duration <= 0
                || request.at ("requestWindow") != 100 || request.at ("submitConcurrency").get<int> () <= 0)
                return {400, R"({"error":"invalid trigger conditions"})"};
            std::lock_guard lock (_gate);
            const auto key = json::array ({run, cell, phase}).dump ();
            if (auto found = _acks.find (key); found != _acks.end ())
                return {200, found->second};
            if (!_ready || _phase != "idle" || (phase == "active" && !_warmed)
                || (phase == "warmup" && _warmed)
                || (!_cell_id.empty () && (_cell_id != cell || _run_id != run)))
                return {409, R"({"error":"source is not ready for this phase"})"};
            // idle is published as the worker's final action; joining it cannot wait on _gate.
            if (_worker.joinable ())
                _worker.join ();
            _run_id = run;
            _cell_id = cell;
            _phase = phase;
            _options.submit_concurrency = request.at ("submitConcurrency").get<int> ();
            _counters.reset ();
            _counters.outstanding = 0;
            const auto ack = json{
              {"accepted", true},
              {"runId", run},
              {"cellId", cell},
              {"phase", phase},
              {"startedAt",
               now_ns ()}}.dump ();
            _acks.emplace (key, ack);
            const auto received_at = std::chrono::duration_cast<std::chrono::milliseconds> (
                                       std::chrono::system_clock::now ().time_since_epoch ())
                                       .count ();
            json trigger = {
              {"runId", run},
              {"cellId", cell},
              {"pattern", _options.pattern},
              {"payloadBytes", _options.payload_size},
              {"durationMs", duration},
              {"submitConcurrency", _options.submit_concurrency},
              {"requestWindow", 100},
              {"warmup", static_cast<int64_t> (_options.warmup_seconds * 1000)},
              {"endpoint",
               "http://127.0.0.1:" + std::to_string (_options.trigger_port) + "/bench/start"},
              {"receivedAtUnixMs", received_at}};
            _worker = std::thread ([this, phase, duration, trigger] {
                try {
                    run_phase (phase, duration, trigger);
                    // Errors and abandoned operations are cell results (spec 5.2: recorded,
                    // excluded from the throughput judgement), not a phase failure. Only an
                    // exception or a readiness timeout fails the phase, as in the .NET source.
                    std::lock_guard lock (_gate);
                    if (phase == "warmup")
                        _warmed = true;
                    _phase = "idle";
                }
                catch (const std::exception &error) {
                    fail (error.what ());
                }
            });
            return {200, ack};
        }
        catch (const std::exception &error) {
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
            _driver->run (clock_t_::now () + std::chrono::milliseconds (duration),
                          _options.payload_size, phase_warmup, _counters, nullptr);
            _warmup_throughput = _driver->warmup_segment_throughput;
            return;
        }
        latency_sampler_t latency (_options.latency_sample_limit);
        cell_t cell;
        cell.implementation = _options.implementation;
        cell.pattern = _options.pattern;
        cell.payload_size = _options.payload_size;
        cell.request_window = _driver->request_window ();
        cell.logical_cores_value = logical_cores ();
        cell.client_parallelism_ceiling = _driver->stream_count ();
        const auto target_before = fetch_stats ("127.0.0.1", _options.target_stats_port);
        if (!target_before)
            throw std::runtime_error ("target stats unavailable before phase");
        const auto cpu_before = process_cpu_seconds_self ();

        const auto start = clock_t_::now ();
        const auto deadline = start + std::chrono::milliseconds (duration);
        long long completed_at_close = 0;
        double elapsed = 0;
        _driver->boundary = [&] {
            // Snapshot before any HTTP work or terminal completion drain.
            elapsed = std::chrono::duration<double> (clock_t_::now () - start).count ();
            completed_at_close = _driver->completed_at_close;
            cell.client_cores = (process_cpu_seconds_self () - cpu_before) / elapsed;
            cell.submit_thread_cores = _driver->submit_cpu_seconds / elapsed;
            cell.non_submit_cores = std::max (0.0, cell.client_cores - cell.submit_thread_cores);
            cell.client_cpu_percent = cell.client_cores / logical_cores () * 100;
            cell.client_memory_mb = rss_mb ();
            cell.client_threads = thread_count_self ();
            const auto boundary = fetch_stats ("127.0.0.1", _options.target_stats_port);
            if (!boundary)
                throw std::runtime_error ("target boundary stats unavailable");
            cell.server_received_at_close = boundary->active_messages;
            cell.server_cpu_percent = (boundary->cpu_seconds - target_before->cpu_seconds) / elapsed
                                      / logical_cores () * 100;
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
        if (_options.pattern != "send-saturation") {
            cell.latency_mean_ms = latency.mean_us () / 1000;
            cell.latency_p95_ms = latency.percentile (0.95) / 1000;
            cell.latency_p99_ms = latency.percentile (0.99) / 1000;
        }
        cell.completed = _counters.completed;
        cell.submitted = _counters.submitted;
        cell.errors = _counters.errors;
        cell.abandoned = _counters.outstanding;
        cell.peak_in_flight = _counters.peak_in_flight;
        cell.header_validation_failures = _counters.header_failures;
        cell.warmup_segment_throughput = _warmup_throughput;
        cell.throughput_per_second = static_cast<double> (_options.pattern == "send-saturation"
                                                            ? cell.server_received_at_close.value ()
                                                            : completed_at_close)
                                     / elapsed;
        cell.bandwidth_mb_s = cell.throughput_per_second * cell.payload_size / 1e6;
        cell.drain_ms =
          std::chrono::duration<double, std::milli> (clock_t_::now () - deadline).count ();
        cell.drain_bound_hit = cell.abandoned != 0;
        const std::map<std::string, std::string> metadata = {
          {"model", "server-driven"},
          {"submitConcurrency", std::to_string (_options.submit_concurrency)},
          {"warmupSeconds", std::to_string (_options.warmup_seconds)},
          {"warmupSegments", std::to_string (_options.warmup_segments)},
          {"warmupUnit", "milliseconds"},
          {"grpcVersion", grpc::Version ()},
          {"grpcServer", "CallbackService; ServerBuilder defaults; insecure loopback; no tuning"},
          {"compiler", __VERSION__},
          {"buildType", "Release"},
          {"frameworkVersion", BENCH_FRAMEWORK_VERSION},
          {"bindingVersion", BENCH_BINDING_VERSION},
          {"coreVersion", BENCH_CORE_VERSION},
          {"protobufVersion", BENCH_PROTOBUF_VERSION}};
        const auto temporary = _options.output_file + ".source";
        write_cells_json (temporary, {cell}, metadata);
        std::ifstream input (temporary);
        auto record = json::parse (input);
        input.close ();
        record["metadata"]["submitConcurrency"] = _options.submit_concurrency;
        auto &value = record["cells"][0];
        value["role"] = "source";
        value["trigger"] = trigger;
        value["streams"] = _driver->streams_metadata ();
        value["completed_at_close"] = completed_at_close;
        value["active_elapsed_ms"] = elapsed * 1000;
        value["client_error_summary"] = _counters.error_summary_json ();
        value["client_error_other_count"] = _counters.other_errors;
        value["server_rejected_count"] = nullptr;
        std::ofstream output (temporary, std::ios::trunc);
        output << record.dump (2) << '\n';
        output.close ();
        if (!output)
            throw std::runtime_error ("cannot write source result");
        std::filesystem::rename (temporary, _options.output_file);
        for (const auto &error : _counters.error_summary)
            std::fprintf (stdout, "client_error: %s: %s (%lld)\n", error.type.c_str (),
                          error.message.c_str (), error.count);
        if (_counters.other_errors != 0)
            std::fprintf (stdout, "client_error_other: %lld\n", _counters.other_errors);
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

std::atomic<bool> stop_source{false};
void source_signal (int)
{
    stop_source = true;
}
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
        options.grpc_endpoint = options.raw_request_endpoint = options.framework_endpoint =
          endpoint;
        options.raw_command_endpoint =
          arg_value (argc, argv, "--command-endpoint", "tcp://127.0.0.1:5288");
        options.trigger_port = std::stoi (arg_value (argc, argv, "--trigger-port", "5280"));
        options.stats_port = std::stoi (arg_value (argc, argv, "--stats-port", "5281"));
        options.target_stats_port =
          std::stoi (arg_value (argc, argv, "--target-stats-port", "5283"));
        options.target_rate = std::stod (arg_value (argc, argv, "--target-rate", "0"));
        if (!std::isfinite (options.target_rate) || options.target_rate < 0)
            throw std::runtime_error ("target rate must be finite and non-negative");
        options.warmup_seconds = std::stod (arg_value (argc, argv, "--warmup-seconds", "5"));
        options.warmup_segments = std::stoi (arg_value (argc, argv, "--warmup-segments", "10"));
        options.request_timeout_ms =
          std::stoi (arg_value (argc, argv, "--request-timeout-ms", "30000"));
        options.drain_bound_ms = std::stoi (arg_value (argc, argv, "--drain-bound-ms", "30000"));
        if (options.warmup_seconds <= 0 || options.warmup_segments < 1
            || options.request_timeout_ms <= 0 || options.drain_bound_ms != 30000)
            throw std::runtime_error ("invalid warmup, timeout or settle bound");
        options.output_file = arg_value (argc, argv, "--output-file", "cells.json");
        source_t source (std::move (options));
        if (!source.start ())
            return 2;
        std::signal (SIGINT, source_signal);
        std::signal (SIGTERM, source_signal);
        while (!stop_source)
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        return 0;
    }
    catch (const std::exception &error) {
        std::fprintf (stderr, "source failed: %s\n", error.what ());
        return 2;
    }
}
