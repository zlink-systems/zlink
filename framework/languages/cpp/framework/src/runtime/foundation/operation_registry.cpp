/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/foundation/operation_registry.hpp"
#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/task_result.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::foundation
{

struct operation_registry_drain_state_t
{
    void admit () noexcept { outstanding.fetch_add (1, std::memory_order_relaxed); }
    void release () noexcept
    {
        if (outstanding.fetch_sub (1, std::memory_order_release) == 1)
            outstanding.notify_all ();
    }
    void wait () noexcept
    {
        auto count = outstanding.load (std::memory_order_acquire);
        while (count != 0) {
            outstanding.wait (count, std::memory_order_acquire);
            count = outstanding.load (std::memory_order_acquire);
        }
    }
    std::atomic<std::size_t> outstanding{0};
};

struct operation_cancellation_t
{
    operation_registry_t *registry;
    call_id_t id;
    void operator() () const noexcept { registry->cancel (id); }
};

struct operation_completion_item_t
{
    explicit operation_completion_item_t (
      operation_registry_t::callback_t callback,
      std::shared_ptr<operation_registry_drain_state_t> drain_state,
      detail::completion_dispatcher_borrow_t dispatcher) :
        callback (std::move (callback)),
        drain_state (std::move (drain_state)),
        dispatcher (std::move (dispatcher))
    {
        this->drain_state->admit ();
    }

    ~operation_completion_item_t ()
    {
        if (drain_state)
            drain_state->release ();
    }

    void finish_metrics (operation_terminal_t terminal) noexcept
    {
        request_metric.complete (terminal);
    }

    std::optional<std::stop_callback<operation_cancellation_t>> cancellation;
    detail::completion_dispatcher_borrow_t dispatcher{};
    operation_registry_t::callback_t callback;
    std::shared_ptr<operation_registry_drain_state_t> drain_state;
    operation_terminal_t terminal = operation_terminal_t::transport_failed;
    zlink::message_t payload;
    std::unique_ptr<operation_completion_item_t> next;
    mesh_request_metric_t request_metric;
};

struct operation_completion_chain_t
{
    void append (std::unique_ptr<operation_completion_item_t> completion,
                 operation_terminal_t terminal,
                 zlink::message_t payload = {},
                 std::optional<operation_terminal_t> request_terminal = {}) noexcept
    {
        completion->finish_metrics (request_terminal.value_or (terminal));
        completion->terminal = terminal;
        completion->payload = std::move (payload);
        auto *next_tail = completion.get ();
        if (tail)
            tail->next = std::move (completion);
        else
            head = std::move (completion);
        tail = next_tail;
        ++size;
    }

    std::unique_ptr<operation_completion_item_t> head;
    operation_completion_item_t *tail = nullptr;
    std::size_t size = 0;
};

namespace
{

runtime::offload_executor_t &isolated_registry_executor ()
{
    static runtime::offload_executor_t executor (1, "zlink-operations");
    return executor;
}

void post_completions (operation_completion_chain_t completions) noexcept
{
    // Stop subscriptions before any callback can release the registry owner.
    // Destroying a stop callback may join it, so this runs outside C1's mutex.
    for (auto *item = completions.head.get (); item; item = item->next.get ())
        item->cancellation.reset ();
    while (completions.head) {
        auto completion = std::move (completions.head);
        completions.head = std::move (completion->next);
        const auto *owner = completion->drain_state.get ();
        // The existing reservation owns the callback and drain lifetime.
        // Capture one raw pointer; the independent turn immediately reclaims it.
        detail::dispatch_completion (
          [item = completion.get ()] {
              // Keep the TLS drain identity valid through callback-capture destruction.
              const auto drain_lifetime = item->drain_state;
              const std::unique_ptr<operation_completion_item_t> completion (item);
              try {
                  completion->callback (completion->terminal, std::move (completion->payload));
              }
              catch (...) {
                  // A consumer failure must still release its drain reservation.
              }
          },
          false, owner, std::move (completion->dispatcher));
        (void) completion.release ();
    }
}

} // namespace

operation_registry_t::operation_registry_t () : operation_registry_t (&clock_t::now)
{
}

operation_registry_t::operation_registry_t (now_t now, std::function<void ()> deadline_changed) :
    _owned_lane (std::make_unique<runtime::state_lane_t> (isolated_registry_executor ())),
    _owner (*_owned_lane),
    _drain_state (std::make_shared<operation_registry_drain_state_t> ()),
    _now (now),
    _deadline_origin (now ()),
    _deadline_changed (std::move (deadline_changed))
{
}

operation_registry_t::operation_registry_t (runtime::state_lane_t &owner,
                                            now_t now,
                                            std::function<void ()> deadline_changed) :
    _owner (owner),
    _drain_state (std::make_shared<operation_registry_drain_state_t> ()),
    _now (now),
    _deadline_origin (now ()),
    _deadline_changed (std::move (deadline_changed))
{
}

operation_registry_t::~operation_registry_t () noexcept
{
    // Late binding observers can outlive the external owner after close.
    // A closed registry has already released its C2 references on that lane.
    {
        std::lock_guard lock (_mutex);
        if (_closed)
            return;
    }
    shutdown ();
}

namespace
{
void notify (std::unique_ptr<operation_completion_item_t> completion,
             operation_terminal_t terminal,
             zlink::message_t payload,
             std::optional<operation_terminal_t> request_terminal = {}) noexcept
{
    operation_completion_chain_t completions;
    completions.append (std::move (completion), terminal, std::move (payload), request_terminal);
    post_completions (std::move (completions));
}
}

bool operation_registry_t::register_operation (call_id_t id,
                                               clock_t::time_point deadline,
                                               callback_t callback,
                                               std::vector<std::uint8_t> target_routing_id,
                                               mesh_request_metric_t request_metric,
                                               std::optional<request_identity_t> request,
                                               std::stop_token cancellation)
{
    if (!callback) {
        throw std::invalid_argument ("operation callback is required");
    }
    if (id.high == 0 && id.low == 0) {
        throw std::invalid_argument ("operation id must not be zero");
    }
    return on_owner ([&] {
        {
            std::lock_guard lock (_mutex);
            if (_closed || _pending.contains (id))
                return false;
        }
        auto pending = std::make_unique<pending_t> ();
        pending->target_routing_id = std::move (target_routing_id);
        pending->request = std::move (request);
        pending->completion = std::make_unique<operation_completion_item_t> (
          std::move (callback), _drain_state, detail::borrow_completion_dispatcher ());
        pending->completion->request_metric = std::move (request_metric);
        if (cancellation.stop_possible ())
            pending->completion->cancellation.emplace (cancellation,
                                                       operation_cancellation_t{this, id});
        {
            std::lock_guard lock (_mutex);
            if (_closed || !_pending.emplace (id, pending.get ()).second)
                return false;
            pending->completion->request_metric.start ();
        }
        const auto earlier = index_deadline (id, deadline, *pending);
        (void) pending.release ();
        if (earlier && _deadline_changed)
            _deadline_changed ();
        // A cancellation between subscription and C1 publication saw no entry.
        if (cancellation.stop_requested ())
            cancel (id);
        return true;
    });
}

std::optional<operation_registry_t::request_identity_t>
operation_registry_t::request_identity (call_id_t id, call_id_t operation) const
{
    std::lock_guard lock (_mutex);
    const auto found = _pending.find (id);
    if (found == _pending.end () || !found->second->request
        || found->second->request->operation != operation)
        return std::nullopt;
    return found->second->request;
}

bool operation_registry_t::index_deadline (const call_id_t &id,
                                           clock_t::time_point deadline,
                                           pending_t &pending)
{
    const auto relative =
      deadline <= _deadline_origin
        ? std::chrono::milliseconds::zero ()
        : std::chrono::ceil<std::chrono::milliseconds> (deadline - _deadline_origin);
    const auto slot = static_cast<std::size_t> (relative.count ()) & (deadline_slot_count - 1);
    pending.id = id;
    pending.deadline = deadline;
    pending.deadline_slot = slot;
    pending.deadline_previous = _deadline_tails[slot];
    if (pending.deadline_previous)
        pending.deadline_previous->deadline_next = &pending;
    else
        _deadline_heads[slot] = &pending;
    _deadline_tails[slot] = &pending;
    ++_index_size;
    const auto reserved = next_deadline ();
    const auto next =
      !reserved
        ? std::min (deadline,
                    _now () + deadline_tick_duration * static_cast<int> (deadline_slot_count))
        : std::min (deadline, *reserved);
    _next_visit.store (next.time_since_epoch ().count (), std::memory_order_release);
    if (_visit_slot < deadline_slot_count)
        _visit_next = std::min (_visit_next, deadline);
    return !reserved || next < *reserved;
}

void operation_registry_t::unindex_deadline (pending_t &pending) noexcept
{
    const auto slot = pending.deadline_slot;
    auto *previous = pending.deadline_previous;
    auto *next = pending.deadline_next;
    if (previous)
        previous->deadline_next = next;
    else
        _deadline_heads[slot] = next;
    if (next)
        next->deadline_previous = previous;
    else
        _deadline_tails[slot] = previous;
    --_index_size;
    delete &pending;
}

bool operation_registry_t::take (const call_id_t &id,
                                 std::unique_ptr<operation_completion_item_t> &completion)
{
    {
        std::lock_guard lock (_mutex);
        const auto found = _pending.find (id);
        if (found == _pending.end ())
            return false;
        completion = std::move (found->second->completion);
        _pending.erase (found);
    }
    completion->cancellation.reset ();
    return true;
}

bool operation_registry_t::complete (const call_id_t &id, zlink::message_t payload)
{
    return complete (id, std::move (payload), {});
}

bool operation_registry_t::complete (const call_id_t &id,
                                     zlink::message_t payload,
                                     before_dispatch_t before_dispatch,
                                     operation_terminal_t request_terminal)
{
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion)) {
        return false;
    }
    try {
        if (before_dispatch)
            before_dispatch ();
    }
    catch (...) {
        payload.close ();
        notify (std::move (completion), operation_terminal_t::transport_failed, {});
        return true;
    }
    notify (std::move (completion), operation_terminal_t::completed, std::move (payload),
            request_terminal);
    return true;
}

namespace
{
task_t<void> finish_completion_async (std::unique_ptr<operation_completion_item_t> completion,
                                      std::function<task_t<void> ()> before_dispatch)
{
    const auto prepared = co_await runtime::await_result (before_dispatch ());
    notify (std::move (completion),
            prepared ? operation_terminal_t::completed : operation_terminal_t::transport_failed,
            {});
}
} // namespace

bool operation_registry_t::complete_async (const call_id_t &id,
                                           std::function<task_t<void> ()> before_dispatch)
{
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion))
        return false;
    // The existing completion reservation retains the drain until preparation ends.
    (void) finish_completion_async (std::move (completion), std::move (before_dispatch));
    return true;
}

bool operation_registry_t::cancel (const call_id_t &id)
{
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion)) {
        return false;
    }
    notify (std::move (completion), operation_terminal_t::cancelled, {});
    return true;
}

bool operation_registry_t::fail (const call_id_t &id,
                                 operation_terminal_t terminal,
                                 zlink::message_t payload)
{
    return fail (id, terminal, std::move (payload), {});
}

bool operation_registry_t::fail (const call_id_t &id,
                                 operation_terminal_t terminal,
                                 zlink::message_t payload,
                                 before_dispatch_t before_dispatch,
                                 std::optional<operation_terminal_t> request_terminal)
{
    if (terminal == operation_terminal_t::completed) {
        throw std::invalid_argument ("failure terminal cannot be completed");
    }
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion)) {
        return false;
    }
    try {
        if (before_dispatch)
            before_dispatch ();
    }
    catch (...) {
        terminal = operation_terminal_t::transport_failed;
        request_terminal.reset ();
        payload.close ();
    }
    notify (std::move (completion), terminal, std::move (payload), request_terminal);
    return true;
}

bool operation_registry_t::unregister (const call_id_t &id)
{
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion))
        return false;
    completion.reset ();
    return true;
}

bool operation_registry_t::contains (const call_id_t &id) const
{
    std::lock_guard lock (_mutex);
    return _pending.contains (id);
}

std::size_t operation_registry_t::fail_target (const std::vector<std::uint8_t> &target_routing_id,
                                               operation_terminal_t terminal)
{
    if (target_routing_id.empty () || terminal == operation_terminal_t::completed)
        throw std::invalid_argument ("target failure requires a target and failure terminal");
    operation_completion_chain_t completions;
    {
        std::lock_guard lock (_mutex);
        for (auto entry = _pending.begin (); entry != _pending.end ();) {
            if (entry->second->target_routing_id != target_routing_id) {
                ++entry;
                continue;
            }
            completions.append (std::move (entry->second->completion), terminal);
            entry = _pending.erase (entry);
        }
    }
    const auto failed = completions.size;
    post_completions (std::move (completions));
    return failed;
}

std::size_t operation_registry_t::expire (clock_t::time_point now)
{
    const auto reserved = next_deadline ();
    if (!reserved || now < *reserved)
        return 0;
    operation_completion_chain_t completions;
    on_owner ([&] {
        const auto visit = next_deadline ();
        if (!visit || now < *visit)
            return;
        const auto started = _now ();
        if (_visit_slot == deadline_slot_count) {
            _visit_next = now + deadline_tick_duration * static_cast<int> (deadline_slot_count);
            _visit_slot = 0;
            _deadline_cursor = _deadline_heads[0];
        }
        // The same index supplies both cleanup and the next deadline. Budget exhaustion
        // retains the cursor and the due reservation; completion never edits either.
        while (_visit_slot < deadline_slot_count) {
            if (!_deadline_cursor) {
                if (++_visit_slot < deadline_slot_count)
                    _deadline_cursor = _deadline_heads[_visit_slot];
                continue;
            }
            if (_now () - started >= runtime::dispatch_limits::receive_batch_time)
                return;
            auto *current = _deadline_cursor;
            _deadline_cursor = current->deadline_next;
            std::unique_ptr<operation_completion_item_t> completion;
            bool live;
            {
                std::lock_guard lock (_mutex);
                const auto found = _pending.find (current->id);
                live = found != _pending.end () && found->second == current;
                if (live && current->deadline <= now) {
                    completion = std::move (current->completion);
                    _pending.erase (found);
                    live = false;
                }
            }
            if (completion)
                completions.append (std::move (completion), operation_terminal_t::timed_out);
            if (!live)
                unindex_deadline (*current);
            else
                _visit_next = std::min (_visit_next, current->deadline);
        }
        _next_visit.store (_index_size ? _visit_next.time_since_epoch ().count ()
                                       : clock_t::duration::max ().count (),
                           std::memory_order_release);
        _visit_slot = deadline_slot_count;
    });
    const auto expired = completions.size;
    post_completions (std::move (completions));
    return expired;
}

std::optional<operation_registry_t::clock_t::time_point>
operation_registry_t::next_deadline () const
{
    const auto ticks = _next_visit.load (std::memory_order_acquire);
    return ticks == clock_t::duration::max ().count ()
             ? std::nullopt
             : std::optional{clock_t::time_point{clock_t::duration{ticks}}};
}

std::size_t operation_registry_t::index_size () const
{
    return on_owner ([&] { return _index_size; });
}

std::size_t operation_registry_t::shutdown ()
{
    bool closed;
    {
        std::lock_guard lock (_mutex);
        closed = _closed;
    }
    if (closed) {
        if (detail::current_completion_owner () != _drain_state.get ())
            _drain_state->wait ();
        return 0;
    }
    operation_completion_chain_t completions;
    std::size_t stopped = 0;
    on_owner ([&] {
        {
            std::lock_guard lock (_mutex);
            if (!_closed) {
                _closed = true;
                for (auto &[id, pending] : _pending) {
                    static_cast<void> (id);
                    completions.append (std::move (pending->completion),
                                        operation_terminal_t::shutdown);
                }
                _pending.clear ();
                stopped = completions.size;
            }
        }
        for (auto *&head : _deadline_heads)
            while (head)
                unindex_deadline (*head);
        _next_visit.store (clock_t::duration::max ().count (), std::memory_order_release);
        _visit_slot = deadline_slot_count;
        _deadline_cursor = nullptr;
        _deadline_changed = {};
    });
    post_completions (std::move (completions));
    // A callback cannot wait for its own reservation. Other completion turns
    // still drain before registry shutdown returns.
    if (detail::current_completion_owner () != _drain_state.get ())
        _drain_state->wait ();
    return stopped;
}

std::size_t operation_registry_t::size () const
{
    std::lock_guard lock (_mutex);
    return _pending.size ();
}

} // namespace zlink::framework::runtime::foundation
