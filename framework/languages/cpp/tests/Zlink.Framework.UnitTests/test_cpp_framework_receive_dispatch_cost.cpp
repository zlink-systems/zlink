/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime_failure_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace
{
using namespace zlink::framework;
using dispatch_t = runtime::mesh::service_mailbox_t::application_dispatch_t;
using pending_t = task_t<std::size_t>;

void require (bool value, const char *message)
{
    if (!value)
        throw std::runtime_error (message);
}

// The former forwarding algorithm. The native owner remains the only pump.
pending_t forwarding_reference (detail::mesh_node_runtime_t &node, dispatch_t dispatch)
{
    co_return co_await node.native_node ().dispatch_ready (std::move (dispatch), false);
}

#if defined(_MSC_VER)
#define RECEIVE_COST_NOINLINE __declspec (noinline)
#else
#define RECEIVE_COST_NOINLINE __attribute__ ((noinline))
#endif

RECEIVE_COST_NOINLINE pending_t empty_management_reference (task_t<void> pending)
{
    co_await pending;
    co_return 0;
}

// Calibrate the required receive result, relay-management result, empty
// infrastructure result and existing local-dispatch lane operation. No
// platform-specific coroutine, promise or control-block size is assumed.
// Use an externally prepared awaitable so compilers cannot replace a coroutine
// that only co_returns with a ready-result factory in the calibration.
RECEIVE_COST_NOINLINE pending_t empty_receive_reference (runtime::state_lane_t &lane,
                                                         task_t<void> management)
{
    (void) co_await empty_management_reference (std::move (management));
    (void) co_await pending_t (result_t<std::size_t>::success (0));
    lane.run ([] {}).get ();
    co_return 0;
}
#undef RECEIVE_COST_NOINLINE

pending_t invoke (detail::mesh_node_runtime_t &node, const dispatch_t &dispatch, int path)
{
    if (path == 0)
        return node.native_node ().dispatch_ready (dispatch, false);
    if (path == 1)
        return forwarding_reference (node, dispatch);
    return node.dispatch_ready (dispatch, false);
}

tests::allocation_measurement_t
measure (detail::mesh_node_runtime_t &node, const dispatch_t &dispatch, int path)
{
    std::optional<pending_t> pending;
    const auto cost =
      tests::measure_allocations ([&] { pending.emplace (invoke (node, dispatch, path)); });
    require (pending->await_ready (), "empty receive rotation unexpectedly suspended");
    require (pending->result ().value () == 0, "empty receive rotation dispatched a record");
    return cost;
}
}

int main ()
{
    completion_owner_t completion_owner;
    try {
        auto state =
          test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("receive-cost");
        state->core_context = std::make_shared<zlink::context_t> ();
        state->listen_endpoint = "tcp://127.0.0.1:5685";
        state->routing_id = zlink::routing_id_t::from ("receive-cost");
        detail::mesh_node_runtime_t node (state);
        node.start ();
        const dispatch_t dispatch = [] (const auto &, const auto &, auto) {
            throw std::runtime_error ("empty receive invoked an application handler");
        };
        for (int path = 0; path != 3; ++path)
            (void) measure (node, dispatch, path);
        std::array<tests::allocation_measurement_t, 3> totals{};
        constexpr int rounds = 16;
        for (int round = 0; round != rounds; ++round) {
            for (int index = 0; index != 3; ++index) {
                const int path = round % 2 ? 2 - index : index;
                const auto cost = measure (node, dispatch, path);
                totals[path].count += cost.count;
                totals[path].bytes += cost.bytes;
                totals[path].largest = std::max (totals[path].largest, cost.largest);
            }
        }
        std::cout << "receive-dispatch rounds=" << rounds;
        for (int path = 0; path != 3; ++path)
            std::cout << " path=" << path << " count=" << totals[path].count
                      << " bytes=" << totals[path].bytes;
        std::cout << '\n';
        runtime::offload_executor_t reference_executor (1);
        runtime::state_lane_t reference_lane (reference_executor);
        task_completion_source_t<void> management;
        management.complete (result_t<void>::success ());
        (void) empty_receive_reference (reference_lane, management.task ()).result ().value ();
        std::optional<pending_t> reference;
        const auto empty_budget = tests::measure_allocations ([&] {
            reference.emplace (empty_receive_reference (reference_lane, management.task ()));
        });
        require (reference->await_ready () && reference->result ().value () == 0,
                 "empty receive calibration did not complete");
        std::cout << "empty-receive budget-count=" << empty_budget.count
                  << " native-count=" << totals[0].count / rounds << '\n';
        require (totals[0].count <= empty_budget.count * rounds,
                 "empty receive allocated an infrastructure processing frame");
        // A receive turn can retain its ordinary/local dispatch records. An
        // empty infrastructure mailbox needs none of its command-specific
        // decoding, relocation or materialization records in that frame.
        const auto receive_storage = sizeof (runtime::host::receive_record_t)
                                     + sizeof (runtime::host::local_application_dispatch_t);
        std::cout << "empty-receive largest=" << totals[0].largest
                  << " receive-storage=" << receive_storage << '\n';
        require (totals[0].largest <= receive_storage,
                 "empty receive retained infrastructure command storage");
        require (totals[1].count > totals[0].count && totals[1].bytes > totals[0].bytes,
                 "forwarding reference failed to expose its task allocation");
        require (totals[2].count <= totals[0].count && totals[2].bytes <= totals[0].bytes,
                 "receive forwarding allocated beyond the native owner baseline");

        auto &mailbox = node.native_node ().transport ().mailbox ();
        constexpr auto infrastructure = runtime::mesh::service_mailbox_domain_t::infrastructure;
        constexpr auto records = runtime::dispatch_limits::receive_batch_messages * 2 + 1;
        for (std::size_t index = 0; index != records; ++index) {
            runtime::mesh::service_mailbox_record_t record;
            record.owner = "receive-cost-infrastructure";
            record.domain = infrastructure;
            record.parts.push_back (zlink::message_t::from (std::vector<std::uint8_t>{0}));
            require (mailbox.try_enqueue (std::move (record)), "infrastructure enqueue failed");
        }
        std::size_t drained = 0;
        while (mailbox.pending_messages (infrastructure) != 0) {
            const auto count = node.dispatch_ready (dispatch, false).result ().value ();
            require (count != 0 && count <= runtime::dispatch_limits::receive_batch_messages,
                     "infrastructure claim was not released or exceeded its batch budget");
            drained += count;
        }
        require (drained == records, "infrastructure claim dropped or duplicated a record");

        if (std::getenv ("ZLINK_CPP_RECEIVE_DISPATCH_TIMING")) {
            constexpr int iterations = 1000;
            for (int round = 0; round != 12; ++round) {
                for (int index = 0; index != 2; ++index) {
                    const int path = round % 2 ? 2 - index : 1 + index;
                    const auto start = std::chrono::steady_clock::now ();
                    for (int i = 0; i != iterations; ++i)
                        require (invoke (node, dispatch, path).result ().value () == 0,
                                 "timed empty rotation dispatched a record");
                    const auto elapsed = std::chrono::duration<double, std::nano> (
                                           std::chrono::steady_clock::now () - start)
                                           .count ();
                    std::cout << "timing round=" << round << " path=" << path
                              << " ns=" << elapsed / iterations << '\n';
                }
            }
        }
        auto rejected = node.dispatch_ready ({});
        require (!rejected.result (), "missing callback unexpectedly succeeded");
        node.stop ();
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}

#ifdef ZLINK_RECEIVE_DISPATCH_COST_DSO
struct receive_allocation_peer_t
{
    bool *tracking;
    std::size_t *count;
    std::size_t *bytes;
    std::size_t *size_filter;
    std::size_t *largest;
};

extern "C" __attribute__ ((visibility ("default"))) void
zlink_receive_allocation_peer (receive_allocation_peer_t *peer)
{
    *peer = {&tests::tracking_allocations, &tests::tracked_allocation_count,
             &tests::tracked_allocation_bytes, &tests::tracked_allocation_size,
             &tests::largest_allocation};
}

namespace
{
class allocation_peers_scope_t
{
  public:
    allocation_peers_scope_t (receive_allocation_peer_t *peers, std::size_t size, bool enabled) :
        _peers (peers), _previous (size)
    {
        for (std::size_t index = 0; index != size; ++index) {
            const auto &peer = _peers[index];
            _previous[index] = {*peer.tracking, *peer.count, *peer.bytes, *peer.size_filter,
                                *peer.largest};
            *peer.count = 0;
            *peer.bytes = 0;
            *peer.size_filter = 0;
            *peer.largest = 0;
            *peer.tracking = enabled;
        }
    }
    ~allocation_peers_scope_t ()
    {
        for (std::size_t index = 0; index != _previous.size (); ++index) {
            const auto &peer = _peers[index];
            *peer.tracking = _previous[index].tracking;
            *peer.count = _previous[index].count;
            *peer.bytes = _previous[index].bytes;
            *peer.size_filter = _previous[index].size_filter;
            *peer.largest = _previous[index].largest;
        }
    }
    tests::allocation_measurement_t measurement () const noexcept
    {
        tests::allocation_measurement_t result{};
        for (std::size_t index = 0; index != _previous.size (); ++index) {
            result.count += *_peers[index].count;
            result.bytes += *_peers[index].bytes;
        }
        return result;
    }

  private:
    struct previous_t
    {
        bool tracking;
        std::size_t count, bytes;
        std::size_t size_filter, largest;
    };
    receive_allocation_peer_t *_peers;
    std::vector<previous_t> _previous;
};
}

// Sum every loaded DSO's caller-thread counter: the shared native allocator
// can bind to the first DSO even while the other Framework owner is running.
extern "C" __attribute__ ((visibility ("default"))) int
zlink_receive_dispatch_measure (std::size_t iterations,
                                int path,
                                bool tracking,
                                receive_allocation_peer_t *peers,
                                std::size_t peer_count,
                                std::size_t *allocations,
                                std::size_t *bytes,
                                double *elapsed_ns)
{
    completion_owner_t completion_owner;
    try {
        auto state =
          test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("receive-cost-dso");
        state->core_context = std::make_shared<zlink::context_t> ();
        state->listen_endpoint = "tcp://127.0.0.1:5686";
        state->routing_id = zlink::routing_id_t::from ("receive-cost-dso");
        detail::mesh_node_runtime_t node (state);
        node.start ();
        const dispatch_t dispatch = [] (const auto &, const auto &, auto) {
            throw std::runtime_error ("empty DSO rotation invoked a handler");
        };
        for (int index = 0; index != 4; ++index)
            require (invoke (node, dispatch, path).result ().value () == 0,
                     "DSO warmup dispatched a record");
        {
            allocation_peers_scope_t scope (peers, peer_count, tracking);
            const auto start = std::chrono::steady_clock::now ();
            for (std::size_t index = 0; index != iterations; ++index) {
                require (invoke (node, dispatch, path).result ().value () == 0,
                         "DSO measurement dispatched a record");
                if (path == 3) {
                    void *(*volatile allocate) (std::size_t) = &::operator new;
                    void *memory = allocate (64);
                    ::operator delete (memory);
                }
            }
            *elapsed_ns =
              std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now () - start)
                .count ();
            const auto measured = scope.measurement ();
            *allocations = measured.count;
            *bytes = measured.bytes;
        }
        node.stop ();
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
#endif
