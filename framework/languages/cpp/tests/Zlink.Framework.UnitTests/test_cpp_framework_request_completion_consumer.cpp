/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include "allocation_counter.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace
{
namespace fw = zlink::framework;
using namespace std::chrono_literals;
constexpr auto test_timeout = 5s;
constexpr const char *mesh_name = "completion-consumer";
constexpr const char *server_endpoint_prefix = "B3_SERVER_ENDPOINT=";
constexpr const char *server_entered = "B3_SERVER_ENTERED";
constexpr const char *client_arm = "B3_CLIENT_ARM";
constexpr const char *client_release = "B3_CLIENT_RELEASE";

void signal_runner (const char *message)
{
    std::cout << message << std::endl;
}
void await_runner (const char *expected)
{
    std::string command;
    if (!std::getline (std::cin, command) || command != expected)
        throw std::runtime_error ("unexpected process coordination command");
}

void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

enum class reply_mode_t
{
    immediate,
    delayed,
    handler_failure
};
struct request_t
{
    static constexpr const char *packet_name = "CompletionConsumerProbe";
    reply_mode_t mode;
};

struct prepared_request_t
{
    static constexpr const char *packet_name = "PreparedCompletionProbe";
    int value;
};
// Only value is serialized. The typed result storage is distinct from the
// small wire value, without inspecting a Framework implementation type.
struct alignas (64) prepared_reply_t
{
    static constexpr std::size_t padding_bytes = 64 * 1024;
    std::array<std::byte, padding_bytes> padding{};
    int value = 0;
};

struct probe_t
{
    std::mutex mutex;
    std::shared_ptr<fw::task_completion_source_t<int>> gate;
    std::atomic<unsigned> encoded{0}, decoded{0}, reply_decoded{0};
    std::atomic<bool> fail_decode{false};
    std::size_t calibrated_result_size = 0;
    unsigned prepared_results_at_encode = 0;

    void arm ()
    {
        std::lock_guard lock (mutex);
        gate = std::make_shared<fw::task_completion_source_t<int>> ();
    }
    void release ()
    {
        std::shared_ptr<fw::task_completion_source_t<int>> pending;
        {
            std::lock_guard lock (mutex);
            pending = gate;
        }
        require (pending->complete (fw::result_t<int>::success (42)),
                 "reply gate was completed twice");
    }
};

struct codec_t
{
    std::shared_ptr<probe_t> probe;
    void register_framework_codecs (fw::codec_registration_context_t &context) const
    {
        context.add_serializer<request_t> (
          [state = probe] (const request_t &request) {
              ++state->encoded;
              return fw::encoded_payload_t::from_string (
                std::to_string (static_cast<int> (request.mode)));
          },
          [state = probe] (const fw::encoded_payload_t &payload) {
              ++state->decoded;
              return request_t{static_cast<reply_mode_t> (std::stoi (payload.to_string ()))};
          },
          "application/x-completion-request");
        context.add_serializer<int> (
          [] (int value) { return fw::encoded_payload_t::from_string (std::to_string (value)); },
          [state = probe] (const fw::encoded_payload_t &payload) {
              ++state->reply_decoded;
              if (state->fail_decode)
                  throw std::runtime_error ("completion consumer decode failure");
              return std::stoi (payload.to_string ());
          },
          "application/x-completion-reply");
        context.add_serializer<prepared_request_t> (
          [state = probe] (const prepared_request_t &request) {
              if (state->calibrated_result_size != 0) {
                  const auto *events = fw::tests::allocation_event_log;
                  require (events != nullptr, "request encoder lost its allocation log");
                  state->prepared_results_at_encode = 0;
                  for (std::size_t index = 0; index < events->count; ++index) {
                      const auto &event = events->events[index];
                      if (event.size == state->calibrated_result_size
                          && reinterpret_cast<std::uintptr_t> (event.memory)
                                 % alignof (prepared_reply_t)
                               == 0)
                          ++state->prepared_results_at_encode;
                  }
              }
              return fw::encoded_payload_t::from_string (std::to_string (request.value));
          },
          [] (const fw::encoded_payload_t &payload) {
              return prepared_request_t{std::stoi (payload.to_string ())};
          },
          "application/x-prepared-request");
        context.add_serializer<prepared_reply_t> (
          [] (const prepared_reply_t &reply) {
              return fw::encoded_payload_t::from_string (std::to_string (reply.value));
          },
          [] (const fw::encoded_payload_t &payload) {
              prepared_reply_t reply;
              reply.value = std::stoi (payload.to_string ());
              return reply;
          },
          "application/x-prepared-reply");
    }
};

struct handler_t
{
    std::shared_ptr<probe_t> probe;
    fw::task_t<int> handle (const request_t &request)
    {
        const auto mode = request.mode;
        std::shared_ptr<fw::task_completion_source_t<int>> gate;
        {
            std::lock_guard lock (probe->mutex);
            gate = probe->gate;
        }
        if (mode == reply_mode_t::handler_failure)
            throw std::runtime_error ("completion consumer handler failure");
        if (mode == reply_mode_t::delayed) {
            signal_runner (server_entered);
            co_return co_await gate->task ();
        }
        co_return 42;
    }
};

struct prepared_handler_t
{
    prepared_reply_t handle (const prepared_request_t &request)
    {
        prepared_reply_t reply;
        reply.value = request.value;
        return reply;
    }
};

struct capture_t final : fw::hosted_service_t
{
    fw::route_client_t *client = nullptr;
    fw::route_mesh_runtime_t *mesh = nullptr;
    fw::framework_runtime_t *runtime = nullptr;
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        client = &services.get_required<fw::route_client_t> ();
        mesh = &services.get_required<fw::route_mesh_runtime_t> ();
        runtime = &services.get_required<fw::framework_runtime_t> ();
        co_return;
    }
    void stop () noexcept override {}
};

struct host_t
{
    fw::app_t app = fw::app_t::create ();
    std::shared_ptr<probe_t> probe = std::make_shared<probe_t> ();
    capture_t *capture;
    zlink::routing_id_t rid;
    std::thread runner;
    std::atomic<bool> exited{false};
    std::exception_ptr run_failure;

    explicit host_t (const char *id, const std::string &peer_endpoint = {}) :
        rid (zlink::routing_id_t::from (id))
    {
        app.logging ().disable_record_capture ();
        auto service = std::make_unique<capture_t> ();
        capture = service.get ();
        app.add_hosted_service (std::move (service));
        auto &options = app.add_zlink_framework ();
        options.codecs ().use (codec_t{probe});
        options.services ().add_singleton<handler_t> (std::make_unique<handler_t> (probe));
        options.services ().add_singleton<prepared_handler_t> (
          std::make_unique<prepared_handler_t> ());
        auto mesh = options.add_route_mesh (mesh_name);
        mesh.set_object_role (fw::object_role_t::none)
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (rid);
        mesh.add_route_request_handler<handler_t, request_t, int> (request_t::packet_name);
        mesh.add_route_request_handler<prepared_handler_t, prepared_request_t, prepared_reply_t> (
          prepared_request_t::packet_name);
        if (!peer_endpoint.empty ())
            mesh.peer_connections ().connect (zlink::routing_id_t::from ("consumer-target"),
                                              peer_endpoint);
    }
    void start ()
    {
        runner = std::thread ([this] {
            try {
                require (app.run (0, nullptr) == 0, "completion consumer app failed");
            }
            catch (...) {
                run_failure = std::current_exception ();
            }
            exited.store (true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now () + test_timeout;
        while (!app.is_ready () && !exited && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        if (exited.load (std::memory_order_acquire) && run_failure)
            std::rethrow_exception (run_failure);
        require (app.is_ready (), "completion consumer app did not become ready");
    }
    void await_peer (const zlink::routing_id_t &target)
    {
        const auto deadline = std::chrono::steady_clock::now () + test_timeout;
        while (std::chrono::steady_clock::now () < deadline) {
            for (const auto &peer : capture->mesh->snapshot (mesh_name).peers)
                if (peer.node_rid == target && peer.state == fw::peer_state_t::ready)
                    return;
            std::this_thread::yield ();
        }
        throw std::runtime_error ("completion consumer peer did not become ready");
    }
    void stop ()
    {
        if (runner.joinable ()) {
            if (probe->gate)
                (void) probe->gate->complete (fw::result_t<int>::success (42));
            app.request_stop ();
            runner.join ();
        }
        if (run_failure)
            std::rethrow_exception (run_failure);
    }
    ~host_t ()
    {
        if (runner.joinable ()) {
            if (probe->gate)
                (void) probe->gate->complete (fw::result_t<int>::success (42));
            app.request_stop ();
            runner.join ();
        }
    }
};

struct continuation_probe_t
{
    std::atomic<unsigned> callbacks{0};
    std::atomic<unsigned> released{0};
};
struct continuation_lifetime_t
{
    std::shared_ptr<continuation_probe_t> probe;
    ~continuation_lifetime_t () { ++probe->released; }
};

fw::task_t<int> observe (fw::request_task_t<int> pending,
                         std::shared_ptr<continuation_probe_t> probe,
                         std::shared_ptr<continuation_lifetime_t> lifetime)
{
    try {
        const auto value = co_await pending;
        ++probe->callbacks;
        lifetime.reset ();
        co_return value;
    }
    catch (...) {
        ++probe->callbacks;
        lifetime.reset ();
        throw;
    }
}

void verify_request (host_t &source,
                     const zlink::routing_id_t &target,
                     reply_mode_t mode,
                     bool fail_decode = false)
{
    if (mode == reply_mode_t::delayed) {
        signal_runner (client_arm);
        await_runner ("armed");
    }
    source.probe->fail_decode = fail_decode;
    const auto decoded = source.probe->reply_decoded.load ();
    const auto encoded = source.probe->encoded.load ();
    auto continuation = std::make_shared<continuation_probe_t> ();
    auto lifetime = std::make_shared<continuation_lifetime_t> (continuation);
    std::optional<fw::request_task_t<int>> pending;
    {
        // The call and its captured request die before a delayed response arrives.
        auto call = source.capture->client->request_to_node (mesh_name, target, request_t{mode});
        pending.emplace (call.timeout (test_timeout).async<int> ());
    }
    auto completed = observe (std::move (*pending), continuation, std::move (lifetime));
    if (mode == reply_mode_t::delayed) {
        await_runner ("entered");
        require (!completed.await_ready (), "delayed reply completed before its gate");
        require (continuation->released == 0, "suspended continuation lost its owner");
        signal_runner (client_release);
    }
    const auto result = completed.result_for (test_timeout);
    require (result.has_value (), "request continuation exceeded the test deadline");
    const bool success = mode != reply_mode_t::handler_failure && !fail_decode;
    if (static_cast<bool> (*result) != success)
        std::fprintf (stderr, "request source=%s target=%s mode=%d decode_failure=%d error=%s\n",
                      source.rid.to_string ().c_str (), target.to_string ().c_str (),
                      static_cast<int> (mode), fail_decode,
                      *result ? "<none>" : result->error ()->what ());
    require (static_cast<bool> (*result) == success, "unexpected request result");
    if (success)
        require (result->value () == 42, "typed request reply changed");
    require (continuation->callbacks == 1, "request continuation did not run exactly once");
    require (continuation->released == 1, "continuation owner was not released exactly once");
    require (source.probe->encoded == encoded + 1, "request was not encoded exactly once");
    const auto reply_count = mode == reply_mode_t::handler_failure ? 0u : 1u;
    require (source.probe->reply_decoded == decoded + reply_count,
             "reply codec did not run exactly once for a physical reply");
    source.probe->fail_decode = false;
}

void verify_results_prepared_before_encode (host_t &source, const zlink::routing_id_t &target)
{
    std::array<fw::tests::allocation_event_t, 256> events{};
    fw::tests::allocation_event_log_t calibration{events.data (), events.size ()};
    std::optional<fw::task_completion_source_t<prepared_reply_t>> reference;
    {
        fw::tests::allocation_event_scope_t scope (calibration);
        (void) fw::tests::count_allocations ([&] { reference.emplace (); });
    }
    require (!calibration.overflow && calibration.count == 1,
             "public typed-result reference did not allocate one result slot");
    const auto reference_size = events[0].size;
    source.probe->calibrated_result_size = reference_size;
    require (reference_size >= sizeof (prepared_reply_t),
             "typed-result reference did not contain its public value storage");
    auto call = source.capture->client->request_to_node (mesh_name, target, prepared_request_t{42});
    std::optional<fw::request_task_t<prepared_reply_t>> pending;
    fw::tests::allocation_event_log_t submission{events.data (), events.size ()};
    fw::tests::allocation_measurement_t measurement;
    {
        fw::tests::allocation_event_scope_t scope (submission);
        measurement = fw::tests::measure_allocations (
          [&] { pending.emplace (call.timeout (test_timeout).async<prepared_reply_t> ()); });
    }
    source.probe->calibrated_result_size = 0;
    const auto result = pending->result_for (test_timeout);
    require (result && *result && result->value ().value == 42,
             "prepared typed-result request failed");
    require (!submission.overflow, "public submission allocation log overflowed");
    std::printf ("prepared public request: messages=1 allocations=%zu bytes=%zu "
                 "reference_size=%zu reference_alignment=%zu slots_at_encode=%u sizes=",
                 measurement.count, measurement.bytes, reference_size, alignof (prepared_reply_t),
                 source.probe->prepared_results_at_encode);
    for (std::size_t index = 0; index < submission.count; ++index)
        std::printf ("%s%zu", index == 0 ? "" : ",", submission.events[index].size);
    std::printf ("\n");
    // Submit/completion §9 requires both public and physical typed-result
    // slots before the request codec runs. This ABI measures its own result
    // storage, and the small wire value cannot require a typed-sized buffer.
    require (source.probe->prepared_results_at_encode == 2,
             "typed results were not prepared before request encoding");
}

}

int main (int argc, char **argv)
{
    try {
        if (argc == 2 && std::string (argv[1]) == "--server") {
            host_t target ("consumer-target");
            target.start ();
            std::cout << server_endpoint_prefix
                      << target.capture->runtime
                           ->listener_status (fw::listener_kind_t::route_mesh, mesh_name)
                           .endpoint
                      << std::endl;
            std::string command;
            while (std::getline (std::cin, command)) {
                if (command == "arm") {
                    target.probe->arm ();
                    signal_runner ("B3_SERVER_ARMED");
                } else if (command == "release") {
                    target.probe->release ();
                } else if (command == "cleanup") {
                    if (target.probe->gate)
                        (void) target.probe->gate->complete (fw::result_t<int>::success (42));
                } else if (command == "stop") {
                    break;
                } else {
                    throw std::runtime_error ("unexpected server coordination command");
                }
            }
            target.stop ();
            require (target.probe->decoded == 4, "server did not decode each request exactly once");
            return 0;
        }
        require (argc == 3 && std::string (argv[1]) == "--client",
                 "expected server or client process mode");
        const auto target = zlink::routing_id_t::from ("consumer-target");
        host_t source ("consumer-source", argv[2]);
        source.start ();
        source.await_peer (target);
        for (auto mode :
             {reply_mode_t::immediate, reply_mode_t::delayed, reply_mode_t::handler_failure})
            verify_request (source, target, mode);
        verify_request (source, target, reply_mode_t::immediate, true);
        std::printf ("public request functionality: remote=4 passed\n");
        verify_results_prepared_before_encode (source, target);
        source.stop ();
    }
    catch (const std::exception &error) {
        std::fprintf (stderr, "%s\n", error.what ());
        return 1;
    }
}
