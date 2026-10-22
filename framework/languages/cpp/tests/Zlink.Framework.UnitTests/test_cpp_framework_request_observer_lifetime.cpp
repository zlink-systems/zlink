/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "allocation_counter.hpp"
#include "scope_exit.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace foundation = zlink::framework::runtime::foundation;
namespace protocol = zlink::framework::runtime::protocol;
using namespace std::chrono_literals;

namespace
{
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

std::vector<std::uint8_t> bytes (const std::string &value)
{
    return {value.begin (), value.end ()};
}

mesh::service_node_descriptor_t descriptor (const char *rid)
{
    return {"observer-lifetime",
            bytes (rid),
            1,
            1,
            "tcp://127.0.0.1:0",
            {{"requests", 100}},
            mesh::service_node_state_t::preparing};
}

std::vector<std::uint8_t> json_bytes ()
{
    return bytes ("\"" + std::string (1022, 'x') + "\"");
}

struct fixture_t
{
    const protocol::application_payload_t reply_payload{"Reply", "application/json", json_bytes ()};
    std::shared_ptr<zlink::context_t> context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t requester{
      zlink::framework::test::runtime_failure_options (
        mesh::raw_mesh_node_options_t{descriptor ("requester")}),
      context};
    mesh::raw_mesh_node_owner_t replier{zlink::framework::test::runtime_failure_options (
                                          mesh::raw_mesh_node_options_t{descriptor ("replier")}),
                                        context};

    fixture_t ()
    {
        requester.start ();
        replier.start ();
        const auto source = requester.topology ().local_descriptor ();
        const auto target = replier.topology ().local_descriptor ();
        replier.expect_peer (source);
        require (requester.connect_peer (replier.endpoint (), target), "connect rejected");
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        for (;;) {
            const auto a = requester.topology ().peer (target.node_routing_id);
            const auto b = replier.topology ().peer (source.node_routing_id);
            if (a && a->ready && b && b->ready)
                break;
            require (std::chrono::steady_clock::now () < deadline, "admission did not complete");
            (void) requester.observe_routes ();
            (void) replier.observe_routes ();
            pump (requester);
            pump (replier);
            std::this_thread::yield ();
        }
    }

    ~fixture_t ()
    {
        requester.close ();
        replier.close ();
    }

    static void pump (mesh::raw_mesh_node_owner_t &owner)
    {
        (void) owner.pump_one (std::chrono::steady_clock::now ()).result ().value ();
    }

    mesh::service_mailbox_claim_t claim ()
    {
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        for (;;) {
            pump (replier);
            auto claim =
              replier.mailbox ().try_claim (mesh::service_mailbox_domain_t::application, 1, 4096);
            if (claim)
                return std::move (*claim);
            require (std::chrono::steady_clock::now () < deadline, "request did not arrive");
            std::this_thread::yield ();
        }
    }

    void deliver (mesh::service_mailbox_claim_t claim)
    {
        require (replier.reply (claim.records.front (), reply_payload), "reply rejected");
        require (replier.mailbox ().release (claim), "claim release failed");
    }

    void wait (const std::atomic_size_t &completed, std::size_t expected)
    {
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (completed.load (std::memory_order_acquire) != expected) {
            require (std::chrono::steady_clock::now () < deadline, "terminal was lost");
            pump (requester);
            std::this_thread::yield ();
        }
    }
};

struct release_marker_t
{
    explicit release_marker_t (std::atomic_size_t &released) : released (released) {}
    ~release_marker_t () { released.fetch_add (1, std::memory_order_release); }
    std::atomic_size_t &released;
};

struct peer_counter_t
{
    bool *tracking = nullptr;
    std::size_t *count = nullptr;
    std::size_t *size = nullptr;
    std::size_t *bytes = nullptr;
    std::size_t *largest = nullptr;
};
thread_local peer_counter_t peer_counter;

template <typename TAction>
zlink::framework::tests::allocation_measurement_t measure_all_allocations (TAction &&action)
{
    struct peer_scope_t
    {
        explicit peer_scope_t (peer_counter_t peer) : peer (peer)
        {
            if (peer.tracking) {
                previous_tracking = *peer.tracking;
                previous_count = *peer.count;
                previous_size = *peer.size;
                previous_bytes = *peer.bytes;
                previous_largest = *peer.largest;
                *peer.count = 0;
                *peer.size = 0;
                *peer.bytes = 0;
                *peer.largest = 0;
                *peer.tracking = true;
            }
        }
        ~peer_scope_t ()
        {
            if (peer.tracking) {
                *peer.tracking = previous_tracking;
                *peer.count = previous_count;
                *peer.size = previous_size;
                *peer.bytes = previous_bytes;
                *peer.largest = previous_largest;
            }
        }
        peer_counter_t peer;
        bool previous_tracking = false;
        std::size_t previous_count = 0;
        std::size_t previous_size = 0;
        std::size_t previous_bytes = 0;
        std::size_t previous_largest = 0;
    } scope (peer_counter);
    auto own = zlink::framework::tests::measure_allocations (std::forward<TAction> (action));
    if (peer_counter.count) {
        own.count += *peer_counter.count;
        own.bytes += *peer_counter.bytes;
        own.largest = std::max (own.largest, *peer_counter.largest);
    }
    return own;
}

struct measurement_t
{
    std::size_t allocations = 0;
    std::size_t bytes = 0;
    std::size_t largest = 0;
    std::uint64_t nanoseconds = 0;
    std::map<std::size_t, std::size_t> histogram;
};

measurement_t verify_pending_reply_after_submission_task_destruction (std::size_t iterations = 1000,
                                                                      bool track = true)
{
    fixture_t fixture;
    std::atomic_size_t completed{0};
    std::atomic_bool callback_failed{false};
    const auto target = fixture.replier.topology ().local_descriptor ().node_routing_id;
    const protocol::application_payload_t payload{"Question", "application/json", json_bytes ()};
    measurement_t measurement;
    zlink::framework::test::scope_exit_t drain ([&] { fixture.requester.close (); });
    for (std::size_t index = 0; index < iterations; ++index) {
        std::optional<zlink::framework::task_t<bool>> submitted;
        auto submit = [&] {
            submitted.emplace (fixture.requester.request_to_node (
              target, payload, 5s,
              [&completed, &payload, &callback_failed] (auto terminal, auto reply) {
                  try {
                      if (terminal != foundation::operation_terminal_t::completed
                          || !reply.valid ())
                          callback_failed.store (true, std::memory_order_release);
                      else {
                          const auto decoded = protocol::decode_application_payload (
                            protocol::message_bytes (reply), false);
                          if (decoded.packet_name != "Reply"
                              || decoded.content_type != "application/json"
                              || decoded.payload_bytes () != payload.payload_bytes ())
                              callback_failed.store (true, std::memory_order_release);
                      }
                  }
                  catch (const std::exception &) {
                      callback_failed.store (true, std::memory_order_release);
                  }
                  completed.fetch_add (1, std::memory_order_release);
              }));
        };
        const auto started = std::chrono::steady_clock::now ();
        if (track) {
            const auto measured = measure_all_allocations (submit);
            measurement.allocations += measured.count;
            measurement.bytes += measured.bytes;
            measurement.largest = std::max (measurement.largest, measured.largest);
            ++measurement.histogram[measured.count];
        } else
            submit ();
        measurement.nanoseconds +=
          static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::nanoseconds> (
                                        std::chrono::steady_clock::now () - started)
                                        .count ());
        require (submitted->result ().value (), "request was not admitted");
        submitted.reset ();
        require (completed.load (std::memory_order_acquire) == index,
                 "reply completed before it was supplied");
        fixture.deliver (fixture.claim ());
        fixture.wait (completed, index + 1);
        require (!callback_failed.load (std::memory_order_acquire),
                 "pending reply terminal or payload changed");
    }
    drain.run ();
    require (completed.load (std::memory_order_acquire) == iterations
               && !callback_failed.load (std::memory_order_acquire),
             "pending request callback failed or repeated");
    if (track) {
        std::cerr << "allocation-histogram";
        for (const auto &[count, requests] : measurement.histogram)
            std::cerr << ' ' << count << ':' << requests;
        std::cerr << '\n';
    }
    return measurement;
}

void verify_late_reply_after_registry_terminal ()
{
    fixture_t fixture;
    std::atomic_size_t completed{0};
    std::atomic_bool callback_failed{false};
    const auto target = fixture.replier.topology ().local_descriptor ().node_routing_id;
    zlink::framework::test::scope_exit_t drain ([&] { fixture.requester.close (); });
    {
        auto submitted = fixture.requester.request_to_node (
          target, {"Question", "application/json", json_bytes ()}, 5s,
          [&completed, &callback_failed] (auto terminal, auto) {
              if (terminal != foundation::operation_terminal_t::timed_out)
                  callback_failed.store (true, std::memory_order_release);
              completed.fetch_add (1, std::memory_order_release);
          });
        require (submitted.result ().value (), "request was not admitted");
    }
    auto claim = fixture.claim ();
    require (fixture.requester.expire_requests (std::chrono::steady_clock::now () + 10s) == 1,
             "operation did not expire");
    fixture.wait (completed, 1);
    const auto receipt = fixture.requester.liveness ().connection (target);
    require (static_cast<bool> (receipt), "request connection receipt is missing");
    const auto before_reply = receipt->deadline.load (std::memory_order_acquire);
    fixture.deliver (std::move (claim));
    const auto receipt_deadline = std::chrono::steady_clock::now () + 5s;
    while (receipt->deadline.load (std::memory_order_acquire) <= before_reply) {
        require (std::chrono::steady_clock::now () < receipt_deadline,
                 "native producer did not observe the late reply");
        fixture_t::pump (fixture.requester);
        std::this_thread::yield ();
    }
    // The binding observer records receipt before delivering the native task.
    // Closing now drains any callback posted by that task's terminal observer.
    drain.run ();
    require (completed.load (std::memory_order_acquire) == 1
               && !callback_failed.load (std::memory_order_acquire),
             "late reply made a second terminal or changed the first terminal");
}

measurement_t verify_ready_native_failure (std::size_t iterations = 1, bool track = true)
{
    const auto context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t requester{
      zlink::framework::test::runtime_failure_options (
        mesh::raw_mesh_node_options_t{descriptor ("ready-requester")}),
      context};
    requester.start ();
    auto missing = descriptor ("no-physical-route");
    // Publish the logical admission through the existing registry surface, with
    // no physical route. The binding must return its initial rejection.
    (void) requester.topology ().admit (missing, bytes ("logical-admission"),
                                        std::chrono::steady_clock::now (),
                                        protocol::command::admit);
    const auto peer = requester.topology ().peer (missing.node_routing_id);
    require (peer && peer->ready, "logical admission fixture failed");
    std::atomic_size_t completed{0};
    std::atomic_bool callback_failed{false};
    measurement_t measurement;
    const protocol::application_payload_t payload{"Question", "application/json", json_bytes ()};
    std::atomic_size_t released{0};
    zlink::framework::test::scope_exit_t drain ([&] { requester.close (); });
    for (std::size_t index = 0; index < iterations; ++index) {
        auto marker = std::make_shared<release_marker_t> (released);
        std::optional<zlink::framework::task_t<bool>> submitted;
        auto submit = [&] {
            submitted.emplace (requester.request_to_node (
              missing.node_routing_id, payload, 5s,
              [&completed, &callback_failed, marker] (auto terminal, auto) {
                  if (terminal == foundation::operation_terminal_t::completed)
                      callback_failed.store (true, std::memory_order_release);
                  completed.fetch_add (1, std::memory_order_release);
              }));
        };
        const auto started = std::chrono::steady_clock::now ();
        if (track) {
            const auto measured = measure_all_allocations (submit);
            measurement.allocations += measured.count;
            measurement.bytes += measured.bytes;
            measurement.largest = std::max (measurement.largest, measured.largest);
            ++measurement.histogram[measured.count];
        } else
            submit ();
        measurement.nanoseconds +=
          static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::nanoseconds> (
                                        std::chrono::steady_clock::now () - started)
                                        .count ());
        require (!submitted->result ().value (), "initial rejection was admitted");
        submitted.reset ();
        marker.reset ();
        const auto terminal_deadline = std::chrono::steady_clock::now () + 5s;
        while (completed.load (std::memory_order_acquire) != index + 1
               || released.load (std::memory_order_acquire) != index + 1) {
            require (std::chrono::steady_clock::now () < terminal_deadline,
                     "ready request terminal was lost");
            std::this_thread::yield ();
        }
    }
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (completed.load (std::memory_order_acquire) != iterations) {
        require (std::chrono::steady_clock::now () < deadline, "ready failure was lost");
        std::this_thread::yield ();
    }
    drain.run ();
    require (completed.load (std::memory_order_acquire) == iterations
               && !callback_failed.load (std::memory_order_acquire),
             "ready request callback failed or repeated");
    if (track) {
        std::cerr << "allocation-histogram";
        for (const auto &[count, requests] : measurement.histogram)
            std::cerr << ' ' << count << ':' << requests;
        std::cerr << '\n';
    }
    return measurement;
}
void verify_early_results ()
{
    const auto context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t requester{
      zlink::framework::test::runtime_failure_options (
        mesh::raw_mesh_node_options_t{descriptor ("early-requester")}),
      context};
    requester.start ();
    std::atomic_size_t callbacks{0};
    const protocol::application_payload_t payload{"Question", "application/json", json_bytes ()};
    const auto target = bytes ("no-logical-peer");
    zlink::framework::test::scope_exit_t drain ([&] { requester.close (); });
    auto invalid = requester.request_to_node (target, payload, 0ms, [&callbacks] (auto, auto) {
        callbacks.fetch_add (1, std::memory_order_release);
    });
    const auto &failure = invalid.result ();
    require (!failure && failure.exception () && failure.error () == nullptr,
             "invalid timeout did not preserve the original exception task");
    bool invalid_argument = false;
    try {
        (void) failure.value ();
    }
    catch (const std::invalid_argument &error) {
        invalid_argument =
          std::string (error.what ()) == "raw mesh request timeout must be positive";
    }
    require (invalid_argument, "invalid timeout changed its exception type or message");
    auto unavailable = requester.request_to_node (target, payload, 5s, [&callbacks] (auto, auto) {
        callbacks.fetch_add (1, std::memory_order_release);
    });
    require (!unavailable.result ().value (), "missing logical peer was admitted");
    drain.run ();
    require (callbacks.load (std::memory_order_acquire) == 0,
             "request rejected before registration invoked a callback");
}

measurement_t verify_all (std::size_t iterations)
{
    void *allocation = nullptr;
    const auto probe =
      zlink::framework::tests::measure_allocations ([&] { allocation = ::operator new (64); });
    ::operator delete (allocation);
    require (probe.count == 1 && probe.bytes == 64 && probe.largest == 64,
             "allocation counter positive probe failed");
    verify_early_results ();
    (void) verify_ready_native_failure ();
    const auto measured = verify_pending_reply_after_submission_task_destruction (iterations);
    verify_late_reply_after_registry_terminal ();
    return measured;
}
} // namespace

#if defined(ZLINK_REQUEST_OBSERVER_DSO)
#if defined(_WIN32)
#define ZLINK_OBSERVER_EXPORT __declspec (dllexport)
#else
#define ZLINK_OBSERVER_EXPORT __attribute__ ((visibility ("default")))
#endif
extern "C" ZLINK_OBSERVER_EXPORT void zlink_request_observer_get_tracker (bool **tracking,
                                                                          std::size_t **count,
                                                                          std::size_t **size,
                                                                          std::size_t **bytes,
                                                                          std::size_t **largest)
{
    *tracking = &zlink::framework::tests::tracking_allocations;
    *count = &zlink::framework::tests::tracked_allocation_count;
    *size = &zlink::framework::tests::tracked_allocation_size;
    *bytes = &zlink::framework::tests::tracked_allocation_bytes;
    *largest = &zlink::framework::tests::largest_allocation;
}

extern "C" ZLINK_OBSERVER_EXPORT void zlink_request_observer_set_peer_tracker (
  bool *tracking, std::size_t *count, std::size_t *size, std::size_t *bytes, std::size_t *largest)
{
    peer_counter = {tracking, count, size, bytes, largest};
}

extern "C" ZLINK_OBSERVER_EXPORT int zlink_request_observer_measure (std::size_t iterations,
                                                                     int ready,
                                                                     int track,
                                                                     std::size_t *allocations,
                                                                     std::uint64_t *nanoseconds,
                                                                     std::size_t *bytes,
                                                                     std::size_t *largest)
{
    try {
        const auto measured =
          ready ? verify_ready_native_failure (iterations, track != 0)
                : verify_pending_reply_after_submission_task_destruction (iterations, track != 0);
        *allocations = measured.allocations;
        *nanoseconds = measured.nanoseconds;
        *bytes = measured.bytes;
        *largest = measured.largest;
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
extern "C" ZLINK_OBSERVER_EXPORT int zlink_request_observer_verify ()
{
    try {
        (void) verify_all (8);
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
#else
int main ()
{
    // Asynchronous completions require a live owner (#1867); it exists before any
    // allocation is measured.
    zlink::framework::completion_owner_t completion_owner;
    const auto measured = verify_all (1000);
    std::cout << "public-request-submit iterations=1000 allocations=" << measured.allocations
              << '\n';
}
#endif
