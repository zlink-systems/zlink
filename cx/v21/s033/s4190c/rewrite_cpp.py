from pathlib import Path
p=Path('framework/bench/grpc/cpp/client/bench_cpp_client.cpp')
s=p.read_text()
a=s.index('class driver_t\n')
b=s.index('int pattern_window (',a)
block=r'''namespace fw = zlink::framework;
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

// Binding continuations resume on its completion poller. No scheduler is bound
// to this coroutine; its captured stream survives a bounded terminal snapshot.
struct binding_observer_t
{
    struct promise_type
    {
        binding_observer_t get_return_object () { return {}; }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_void () noexcept {}
        void unhandled_exception () noexcept { std::terminate (); }
    };
};

template <typename T, typename Success, typename Failure>
binding_observer_t observe_binding (zlink::async_result_t<T> result,
                                    Success success, Failure failure)
{
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move (result);
            success ();
        } else {
            auto value = co_await std::move (result);
            success (value);
        }
    } catch (const std::exception &error) {
        failure (error);
    }
}

class driver_t
{
  public:
    driver_t (const options_t &options, int window, bool command) :
        _options (options), _window (window), _command (command) {}
    virtual ~driver_t () = default;
    int request_window () const { return _window; }
    std::function<void ()> boundary;
    long long completed_at_close = 0;
    double submit_cpu_seconds = 0;
    void close_window (clock_t_::time_point deadline)
    {
        if (boundary && clock_t_::now () >= deadline) {
            auto sample = std::move (boundary);
            sample ();
        }
    }
    virtual bool await_ready (int timeout_ms) = 0;

    void run (clock_t_::time_point deadline, size_t size, phase_t phase,
              counters_t &counters, latency_sampler_t *latency)
    {
        const int count = _options.pattern == "request-serial" ? 1 : _options.submit_concurrency;
        prepare_streams (count);
        std::vector<std::shared_ptr<stream_t>> streams;
        std::vector<std::thread> threads;
        std::latch submitted_done (count);
        std::latch boundary_done (1);
        for (int index = 0; index < count; ++index) {
            auto stream = std::make_shared<stream_t> (
              latency ? _options.latency_sample_limit : 0, deadline, _run_id, phase, size);
            streams.push_back (stream);
            threads.emplace_back ([&, stream, index] {
                const auto name = "bench-submit-" + std::to_string (index);
                pthread_setname_np (pthread_self (), name.c_str ());
                const double cpu_start = thread_cpu_seconds_self ();
                submission_pacer_t pacer (_options.target_rate / count);
                uint64_t sequence = static_cast<uint64_t> (index) << 56;
                while (clock_t_::now () < deadline) {
                    {
                        std::unique_lock lock (stream->mutex);
                        stream->changed.wait_until (lock, deadline, [&] {
                            return stream->stopped || (stream->admitted
                              && (_window <= 0 || stream->counters.outstanding < _window));
                        });
                        if (stream->stopped || clock_t_::now () >= deadline)
                            break;
                    }
                    if (_options.target_rate > 0 && pacer.wait_for_due_slots (deadline) == 0)
                        break;
                    {
                        std::lock_guard lock (stream->mutex);
                        stream->admitted = false;
                        stream->counters.enter ();
                        ++stream->counters.submitted;
                    }
                    try {
                        submit (stream, index, sequence++);
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
        std::this_thread::sleep_until (deadline);
        submitted_done.wait ();
        completed_at_close = 0;
        submit_cpu_seconds = 0;
        for (const auto &stream : streams) {
            std::lock_guard lock (stream->mutex);
            completed_at_close += stream->active_completed;
            submit_cpu_seconds += stream->submit_cpu;
        }
        close_window (deadline);
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
            for (const auto &error : from.error_summary) {
                // Preserve counts without replaying each individual failure.
                auto found = std::find_if (counters.error_summary.begin (), counters.error_summary.end (),
                  [&] (const auto &entry) { return entry.type == error.type && entry.message == error.message; });
                if (found != counters.error_summary.end ())
                    found->count += error.count;
                else if (counters.error_summary.size () < counters_t::error_kind_limit)
                    counters.error_summary.push_back (error);
                else
                    counters.other_errors += error.count;
            }
            counters.errors += from.errors.load ();
            counters.other_errors += from.other_errors;
            if (latency)
                latency->merge (stream->latency);
        }
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
};

class grpc_driver_t final : public driver_t
{
  public:
    grpc_driver_t (const options_t &options, int window, bool command) :
        driver_t (options, window, command),
        _channel (grpc::CreateChannel (options.grpc_endpoint, grpc::InsecureChannelCredentials ())) {}
    bool await_ready (int timeout_ms) override
    {
        return _channel->WaitForConnected (std::chrono::system_clock::now ()
                                          + std::chrono::milliseconds (timeout_ms));
    }

  private:
    void prepare_streams (int count) override
    {
        _stubs.clear ();
        for (int i = 0; i < count; ++i)
            _stubs.push_back (zlink::framework::bench::withgrpc::BenchService::NewStub (_channel));
    }
    template <typename Reply> struct call_t
    {
        grpc::ClientContext context;
        payload_t request;
        Reply reply;
    };
    template <typename Reply>
    void start (std::shared_ptr<stream_t> stream, int index, uint64_t seq)
    {
        auto call = std::make_shared<call_t<Reply>> ();
        call->context.set_deadline (std::chrono::system_clock::now ()
                                   + std::chrono::milliseconds (_options.request_timeout_ms));
        call->request = make_payload (stream->size, stream->phase, seq);
        const auto sent = now_ns ();
        auto completion = [stream, call, sent, seq] (grpc::Status status) {
            if (!status.ok ()) {
                const std::runtime_error error ("gRPC code " + std::to_string (status.error_code ())
                                                + ": " + status.error_message ());
                stream->finish (0, nullptr, seq, &error);
            } else {
                const payload_t *reply = nullptr;
                if constexpr (std::is_same_v<Reply, payload_t>)
                    reply = &call->reply;
                stream->finish (sent, reply, seq);
            }
        };
        if constexpr (std::is_same_v<Reply, payload_t>)
            _stubs[index]->async ()->Echo (&call->context, &call->request, &call->reply, completion);
        else
            _stubs[index]->async ()->Command (&call->context, &call->request, &call->reply, completion);
        stream->admit ();
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
};

class zlink_raw_driver_t final : public driver_t
{
  public:
    zlink_raw_driver_t (const options_t &options, int window, bool command) :
        driver_t (options, window, command), _socket (_context)
    {
        _socket.set_routing_id (zlink::routing_id_t::from (
          "bench-source-" + std::to_string (::getpid ())));
        _target = zlink::routing_id_t::from (command ? options.raw_command_rid : options.raw_request_rid);
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
            catch (const std::exception &) { return false; }
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
        const auto sent = now_ns ();
        if (_command) {
            auto submission = _socket.send (_target).message (header).message (body).async ();
            observe_binding (std::move (submission.admitted),
              [stream, seq] { stream->finish (0, nullptr, seq); stream->admit (); },
              [stream, seq] (const auto &error) { stream->finish (0, nullptr, seq, &error); stream->admit (false); });
        } else {
            auto submission = _socket.request (_target).message (header).message (body)
              .timeout (std::chrono::milliseconds (_options.request_timeout_ms)).async ();
            observe_binding (std::move (submission.reply),
              [stream, sent, seq] (const auto &parts) {
                  payload_t reply;
                  if (parts.empty () || !reply.ParseFromArray (parts.back ().data (), static_cast<int> (parts.back ().size ()))) {
                      const std::runtime_error error ("invalid raw reply protobuf");
                      stream->finish (0, nullptr, seq, &error);
                  } else
                      stream->finish (sent, &reply, seq);
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
        auto payload = make_payload (stream->size, stream->phase, seq);
        if (_command) {
            auto task = _client->send_to_node ("bench", _target_node, std::move (payload)).async ();
            fw::detail::observe_task_completion (task, [stream, seq] (const auto &result) {
                stream->finish (0, nullptr, seq, result ? nullptr : result.error ().get ());
                stream->admit (static_cast<bool> (result));
            });
        } else {
            const auto sent = now_ns ();
            auto task = _client->request_to_node ("bench", _target_node, std::move (payload))
              .timeout (std::chrono::milliseconds (_options.request_timeout_ms)).async<payload_t> ();
            fw::detail::observe_task_completion (task, [stream, sent, seq] (const auto &result) {
                stream->finish (sent, result ? &result.value () : nullptr, seq,
                                result ? nullptr : result.error ().get ());
            });
            auto admission = task.admitted ();
            fw::detail::observe_task_completion (admission, [stream] (const auto &result) {
                stream->admit (static_cast<bool> (result));
            });
        }
    }
    stats_http_server_t _host;
    zlink::routing_id_t _target_node = zlink::routing_id_t::from ("bench-server");
    fw::route_client_t *_client = nullptr;
};

'''
s=s[:a]+block+s[b:]
s=s.replace('#include <condition_variable>','#include <condition_variable>\n#include <latch>\n#include <pthread.h>')
s=s.replace('double target_rate = 0;', 'double target_rate = 0;\n    int submit_concurrency = 8;')
s=s.replace('if (pattern == "send-saturation")\n        return 8;', 'if (pattern == "send-saturation")\n        return 1;')
s=s.replace('sendConcurrency','submitConcurrency')
s=s.replace('request.at ("submitConcurrency") != 8', '(request.at ("submitConcurrency").get<int> () <= 0 || request.at ("submitConcurrency").get<int> () > 128)')
s=s.replace('_counters.reset ();', '_options.submit_concurrency = request.at ("submitConcurrency").get<int> ();\n            _counters.reset ();')
s=s.replace('const auto submit_before = thread_cpu_seconds_self ();','')
s=s.replace('completed_at_close = _counters.completed;', 'completed_at_close = _driver->completed_at_close;')
s=s.replace('(thread_cpu_seconds_self () - submit_before) / elapsed', '_driver->submit_cpu_seconds / elapsed')
for line in ['            cell.latency_mean_ms = latency.mean_us () / 1000;\n','            cell.latency_p95_ms = latency.percentile (0.95) / 1000;\n','            cell.latency_p99_ms = latency.percentile (0.99) / 1000;\n']:
 s=s.replace(line,'')
s=s.replace('        cell.completed = _counters.completed;', '''        if (_options.pattern != "send-saturation") {
            cell.latency_mean_ms = latency.mean_us () / 1000;
            cell.latency_p95_ms = latency.percentile (0.95) / 1000;
            cell.latency_p99_ms = latency.percentile (0.99) / 1000;
        }
        cell.completed = _counters.completed;''')
s=s.replace('{"model", "server-driven"},','{"model", "server-driven"},\n          {"submitConcurrency", std::to_string (_options.submit_concurrency)},')
s=s.replace('_options.pattern == "send-saturation" ? 8 : 1','_options.pattern == "request-serial" ? 1 : _options.submit_concurrency')
a=s.index('          {"implementation",\n',s.index('value["streams"]'))
b=s.index('}};',a)+3
s=s[:a]+'''          {"implementation", "one dedicated submit thread per stream; native completion resources"}};'''+s[b:]
p.write_text(s)
p=Path('framework/bench/grpc/cpp/common/bench_common.hpp')
s=p.read_text().replace('    void reset ()\n    {\n        _sum', '''    void merge (const latency_sampler_t &other)
    {
        _sum += other._sum;
        _count += other._count;
        const auto count = std::min (_cap - _samples.size (), other._samples.size ());
        _samples.insert (_samples.end (), other._samples.begin (), other._samples.begin () + count);
    }

    void reset ()
    {
        _sum''')
p.write_text(s)
p=Path('framework/bench/grpc/runner_common.sh');s=p.read_text().replace('SEND_CONCURRENCY=8','SUBMIT_CONCURRENCY=8').replace('\\"sendConcurrency\\":${SEND_CONCURRENCY}','\\"submitConcurrency\\":${SUBMIT_CONCURRENCY}');p.write_text(s)
p=Path('framework/bench/grpc/cpp/CMakeLists.txt');s=p.read_text().replace('find_package(zlink_cpp ${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION} EXACT CONFIG REQUIRED)', '''if(NOT ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE)
  find_package(zlink_cpp ${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION} EXACT CONFIG REQUIRED)
endif()''');p.write_text(s)
