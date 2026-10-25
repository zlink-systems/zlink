/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include "test_completion_poller_driver.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace
{
constexpr std::size_t ignored_bytes = 128 * 1024;
constexpr std::size_t large_threshold = ignored_bytes / 2;
std::atomic_bool measuring{false};
std::atomic_size_t large_allocations{0};

void count (std::size_t size) noexcept
{
    if (size >= large_threshold && measuring.load (std::memory_order_relaxed))
        large_allocations.fetch_add (1, std::memory_order_relaxed);
}
void *allocate (std::size_t size)
{
    count (size);
    if (void *memory = std::malloc (size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc ();
}
void *allocate_aligned (std::size_t size, std::size_t alignment)
{
    count (size);
#ifdef _WIN32
    void *memory = _aligned_malloc (size == 0 ? 1 : size, alignment);
#else
    const auto rounded = ((size == 0 ? 1 : size) + alignment - 1) / alignment * alignment;
    void *memory = std::aligned_alloc (alignment, rounded);
#endif
    if (memory)
        return memory;
    throw std::bad_alloc ();
}
void free_aligned (void *memory) noexcept
{
#ifdef _WIN32
    _aligned_free (memory);
#else
    std::free (memory);
#endif
}
} // namespace

// This executable observes all Framework workers. The shared allocation helper
// is thread-local and cannot measure a public request's receive/completion work.
void *operator new (std::size_t size)
{
    return allocate (size);
}
void *operator new[] (std::size_t size)
{
    return allocate (size);
}
void operator delete (void *memory) noexcept
{
    std::free (memory);
}
void operator delete[] (void *memory) noexcept
{
    std::free (memory);
}
void operator delete (void *memory, std::size_t) noexcept
{
    std::free (memory);
}
void operator delete[] (void *memory, std::size_t) noexcept
{
    std::free (memory);
}
void *operator new (std::size_t size, std::align_val_t alignment)
{
    return allocate_aligned (size, static_cast<std::size_t> (alignment));
}
void *operator new[] (std::size_t size, std::align_val_t alignment)
{
    return allocate_aligned (size, static_cast<std::size_t> (alignment));
}
void operator delete (void *memory, std::align_val_t) noexcept
{
    free_aligned (memory);
}
void operator delete[] (void *memory, std::align_val_t) noexcept
{
    free_aligned (memory);
}
void operator delete (void *memory, std::size_t, std::align_val_t) noexcept
{
    free_aligned (memory);
}
void operator delete[] (void *memory, std::size_t, std::align_val_t) noexcept
{
    free_aligned (memory);
}

namespace
{
namespace fw = zlink::framework;
using namespace std::chrono_literals;
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

struct probe_request_t
{
    static constexpr const char *packet_name = "probe";
    std::string value;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (probe_request_t, value)
};

struct handler_t
{
    using request_type = probe_request_t;
    using reply_type = std::string;
    static constexpr const char *topic_name = "";
    static inline std::atomic_uint calls{0};
    std::string handle (const probe_request_t &value)
    {
        ++calls;
        return value.value;
    }
};

class running_app_t
{
  public:
    explicit running_app_t (fw::app_t &app) :
        _app (app), _run (std::async (std::launch::async, [&app] {
            char name[] = "envelope-public-allocations";
            char *argv[] = {name, nullptr};
            return app.run (1, argv);
        }))
    {
    }
    ~running_app_t ()
    {
        _app.stop ();
        if (_run.valid ())
            _run.wait ();
    }
    void finish ()
    {
        _app.stop ();
        require (_run.get () == 0, "public App returned a failure exit code");
    }
    void wait_ready ()
    {
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (!_app.is_ready () && std::chrono::steady_clock::now () < deadline) {
            require (_run.wait_for (0ms) != std::future_status::ready, "public App stopped early");
            std::this_thread::yield ();
        }
        require (_app.is_ready (), "public App did not become ready");
    }

  private:
    fw::app_t &_app;
    std::future<int> _run;
};

fw::task_t<std::vector<zlink::message_t>>
await_reply (zlink::async_result_t<std::vector<zlink::message_t>> pending)
{
    co_return co_await std::move (pending);
}

void verify_public_decode ()
{
    auto app = fw::app_t::create ();
    app.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::off);
        options.add_client_server_channel ("alloc")
          .server ()
          .set_bind_host ("127.0.0.1")
          .set_advertise_host ("127.0.0.1")
          .listen ()
          .add_request_handler<handler_t, probe_request_t, std::string> ();
    });
    auto services = app.advanced ().services ().build_provider ();
    running_app_t running (app);
    running.wait_ready ();
    const auto endpoint = services.get_required<fw::framework_runtime_t> ()
                            .listener_status (fw::listener_kind_t::client_server, "alloc")
                            .endpoint;
    zlink::context_t context;
    zlink::dealer_socket_t source (context);
    source.set_routing_id (zlink::routing_id_t::from ("allocation-public-peer"));
    source.options ().linger (0ms);
    source.options ().recv_timeout (5s);
    fw::test::completion_poller_driver_t completions (source);
    source.connect (endpoint);
    // service-wire-v1.schema.json#/commands/0 and #/types/122:
    // channel="alloc", direction=1, identity="default", max message=16MiB.
    const std::vector<std::uint8_t> hello{90,  77,  1,   1,   0,   2,   0,   0,   0, 22, 1,
                                          0,   19,  5,   'a', 'l', 'l', 'o', 'c', 1, 7,  'd',
                                          'e', 'f', 'a', 'u', 'l', 't', 1,   0,   0, 0};
    const auto admitted =
      await_reply (
        source.request ().message (zlink::message_t::from (hello)).timeout (5s).async ().reply)
        .result ();
    require (admitted && admitted.value ().size () == 1, "raw public peer was not admitted");
    zlink::received_t probe;
    require (source.recv (probe) == 0, "public server did not send its liveness probe");
    auto ack = probe.parts ().front ().to_bytes ();
    // service-wire-v1.schema.json#/protocol/headPrefixBytes, #/commands/4 and /5:
    // livenessProbe(5)/livenessAck(6) share the same opaque u64 probe id.
    constexpr std::size_t command_offset = 3;
    constexpr std::size_t head_prefix_bytes = 5;
    constexpr std::size_t liveness_record_bytes = head_prefix_bytes + sizeof (std::uint64_t);
    constexpr std::uint8_t liveness_probe = 5;
    constexpr std::uint8_t liveness_ack = 6;
    require (ack.size () == liveness_record_bytes && ack[command_offset] == liveness_probe,
             "unexpected initial liveness record");
    ack[command_offset] = liveness_ack;
    (void) source.send ().message (zlink::message_t::from (ack)).async ();

    const std::string prefix =
      R"({"formatMarker":242,"kind":1,"channelName":"alloc","messageName":"probe","contentType":"application/json","correlationId":"allocation-probe")";
    const auto short_header = zlink::message_t::from (prefix + "}");
    const auto unknown_header = zlink::message_t::from (prefix + ",\"unknown\":\""
                                                        + std::string (ignored_bytes, 'u') + "\"}");
    const auto flow_header = zlink::message_t::from (
      prefix + ",\"flowId\":\"" + std::string (ignored_bytes, 'f') + "\",\"flowOrigin\":{} }");
    fw::serializer_registry_t serializers;
    const auto encoded = serializers.get<probe_request_t> ().serialize (probe_request_t{"ok"});
    const auto body = zlink::message_t::from (encoded.to_string ());
    const auto matches_reply = [&] (const auto &reply) {
        return reply && reply.value ().size () == 2
               && serializers.get<std::string> ().deserialize (
                    fw::encoded_payload_t::from_string (reply.value ()[1].to_string ()))
                    == "ok";
    };
    const auto request = [&] (const zlink::message_t &header) {
        return await_reply (source.request ()
                              .message (zlink::message_t (header))
                              .message (zlink::message_t (body))
                              .timeout (5s)
                              .async ()
                              .reply)
          .result ();
    };
    for (unsigned index = 0; index < 8; ++index)
        require (matches_reply (request (short_header)), "public request warmup failed");
    bool allocation_bound_met = true;
    for (const auto *header : {&short_header, &unknown_header, &flow_header}) {
        const auto before = handler_t::calls.load ();
        large_allocations.store (0);
        measuring.store (true);
        const auto reply = request (*header);
        measuring.store (false);
        const auto allocations = large_allocations.load ();
        std::cout << "public envelope bytes=" << header->size ()
                  << " large allocations=" << allocations << '\n';
        if (!reply)
            std::cerr << "public reply failed: " << reply.error ()->what () << '\n';
        else if (!matches_reply (reply)) {
            std::cerr << "public reply parts=" << reply.value ().size () << '\n';
            for (const auto &part : reply.value ())
                std::cerr << "reply part=" << part.to_string () << '\n';
        }
        require (matches_reply (reply),
                 "measured public request did not return the typed handler's reply");
        require (handler_t::calls.load () == before + 1, "measured request skipped public handler");
        allocation_bound_met = allocation_bound_met && allocations == 0;
    }
    running.finish ();
    require (allocation_bound_met, "ignored envelope fields allocate owning parser storage");
}
} // namespace

int main ()
{
    try {
        // An allocation on another thread must be observed, including when a
        // compiler could otherwise elide an unused new/delete expression.
        large_allocations.store (0);
        measuring.store (true);
        std::thread injected ([] {
            void *(*volatile allocate_probe) (std::size_t) = &::operator new;
            void *memory = allocate_probe (ignored_bytes);
            ::operator delete (memory);
        });
        injected.join ();
        measuring.store (false);
        require (large_allocations.load () == 1,
                 "cross-thread allocation counter missed injected copy");
        large_allocations.store (0);
        void *(*volatile allocate_probe) (std::size_t) = &::operator new;
        void *memory = allocate_probe (ignored_bytes);
        ::operator delete (memory);
        measuring.store (true);
        memory = allocate_probe (large_threshold - 1);
        ::operator delete (memory);
        measuring.store (false);
        require (large_allocations.load () == 0, "probe counted disabled or small allocations");
        std::cout << "injected cross-thread allocation: detected\n";
        verify_public_decode ();
        return 0;
    }
    catch (const std::exception &error) {
        measuring.store (false);
        std::cerr << error.what () << '\n';
        return 1;
    }
}
