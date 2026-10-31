/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/foundation/operation_registry.hpp"
#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/task_result.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/detail/deadline_scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::foundation
{

class operation_registry_state_t : public std::enable_shared_from_this<operation_registry_state_t>
{
  public:
    using clock_t = std::chrono::steady_clock;
    using callback_t = std::function<void (operation_terminal_t, result_t<zlink::message_t>)>;
    using before_dispatch_t = std::function<void ()>;
    // The management budget reads this clock; tests inject a deterministic one.
    using now_t = clock_t::time_point (*) ();

    using request_identity_t = operation_registry_t::request_identity_t;
    std::optional<request_identity_t> request_identity (call_id_t id, call_id_t operation) const;

    explicit operation_registry_state_t (now_t now, std::function<void ()> deadline_changed = {});
    explicit operation_registry_state_t (runtime::state_lane_t &owner,
                                         now_t now = &clock_t::now,
                                         std::function<void ()> deadline_changed = {});
    ~operation_registry_state_t () noexcept;

    bool register_operation (call_id_t id,
                             clock_t::time_point deadline,
                             callback_t callback,
                             std::vector<std::uint8_t> target_routing_id = {},
                             mesh_request_metric_t request_metric = {},
                             std::optional<request_identity_t> request = {},
                             std::stop_token cancellation = {});
    bool contains (const call_id_t &id) const;
    std::size_t fail_target (const std::vector<std::uint8_t> &target_routing_id,
                             operation_terminal_t terminal);
    bool complete (const call_id_t &id, result_t<zlink::message_t> payload);
    // Packed infrastructure replies retain their delivery terminal and payload
    // for the caller to decode; request_terminal observes their semantic result.
    bool complete (const call_id_t &id,
                   result_t<zlink::message_t> payload,
                   before_dispatch_t before_dispatch,
                   operation_terminal_t request_terminal = operation_terminal_t::completed);
    bool complete_async (const call_id_t &id, std::function<task_t<void> ()> before_dispatch);
    bool cancel (const call_id_t &id);
    bool fail (const call_id_t &id,
               operation_terminal_t terminal,
               result_t<zlink::message_t> payload = result_t<zlink::message_t>::success ({}));
    bool fail (const call_id_t &id,
               operation_terminal_t terminal,
               result_t<zlink::message_t> payload,
               before_dispatch_t before_dispatch,
               std::optional<operation_terminal_t> request_terminal = {});
    bool unregister (const call_id_t &id);
    task_t<std::size_t> expire (clock_t::time_point now);
    std::size_t expire_on_owner (clock_t::time_point now);
    // An already reserved visit survives terminal removal, including the last entry.
    std::optional<clock_t::time_point> next_deadline () const;
    std::size_t shutdown ();
    std::size_t size () const;
    std::size_t index_size () const;

  public:
    using id_hash_t = runtime::call_id_hash_t;

    static constexpr std::size_t deadline_slot_count = 256;
    static constexpr auto deadline_tick_duration = std::chrono::milliseconds (1);

    struct pending_t
    {
        std::unique_ptr<operation_completion_item_t> completion;
        std::vector<std::uint8_t> target_routing_id;
        call_id_t id;
        clock_t::time_point deadline;
        std::size_t deadline_slot = deadline_slot_count;
        pending_t *deadline_previous = nullptr;
        pending_t *deadline_next = nullptr;
        std::optional<request_identity_t> request;
    };

    template <typename Map> static auto find_pending (Map &pending, const call_id_t &id)
    {
        const auto [begin, end] = pending.equal_range (id);
        const auto found = std::find_if (
          begin, end, [] (const auto &entry) { return bool (entry.second.completion); });
        return found == end ? pending.end () : found;
    }
    // C1's single terminal take; an empty completion is the only settled marker.
    std::unique_ptr<operation_completion_item_t> take_completion (pending_t &pending)
    {
        auto completion = std::exchange (pending.completion, {});
        if (completion)
            --_pending_count;
        return completion;
    }
    bool take (const call_id_t &id, std::unique_ptr<operation_completion_item_t> &completion);
    bool index_deadline (const call_id_t &id, clock_t::time_point deadline, pending_t &pending);
    void unindex_deadline (pending_t &pending) noexcept;
    void clear_deadlines () noexcept;
    void reserve_deadline_visit (std::weak_ptr<operation_registry_state_t> self);

    runtime::state_lane_t *_owner = nullptr;
    std::shared_ptr<operation_registry_drain_state_t> _drain_state;
    mutable std::mutex _mutex;
    // C1 owns completion take. A taken entry remains as index storage until
    // management visits it; a reused identity may coexist with its taken entry.
    std::unordered_multimap<call_id_t, pending_t, id_hash_t> _entries;
    // Observation only: terminal authority remains the completion slot above.
    // Map size also includes taken entries retained solely as index storage.
    std::size_t _pending_count = 0;
    const now_t _now;
    const clock_t::time_point _deadline_origin;
    std::function<void ()> _deadline_changed;
    std::shared_ptr<detail::deadline_state_t> _deadline_visit;
    std::array<pending_t *, deadline_slot_count> _deadline_heads{};
    std::array<pending_t *, deadline_slot_count> _deadline_tails{};
    std::atomic_size_t _index_size{0};
    // The owner's single reservation is also the polling resource's wake snapshot.
    std::atomic<clock_t::duration::rep> _next_visit{clock_t::duration::max ().count ()};
    // Minimum deadline found in the current budgeted scan, not the published wake.
    clock_t::time_point _scan_minimum_deadline;
    std::size_t _visit_slot = deadline_slot_count;
    pending_t *_deadline_cursor = nullptr;
    bool _closed = false;
};


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
    operation_registry_state_t *registry;
    call_id_t id;
    void operator() () const noexcept { registry->cancel (id); }
};

struct operation_completion_item_t
{
    explicit operation_completion_item_t (
      operation_registry_state_t::callback_t callback,
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
        callback = nullptr;
        if (drain_state)
            drain_state->release ();
    }

    void finish_metrics (operation_terminal_t terminal) noexcept
    {
        request_metric.complete (terminal);
    }

    std::optional<std::stop_callback<operation_cancellation_t>> cancellation;
    detail::completion_dispatcher_borrow_t dispatcher{};
    operation_registry_state_t::callback_t callback;
    std::shared_ptr<operation_registry_drain_state_t> drain_state;
    operation_terminal_t terminal = operation_terminal_t::transport_failed;
    result_t<zlink::message_t> payload = result_t<zlink::message_t>::success ({});
    std::unique_ptr<operation_completion_item_t> next;
    mesh_request_metric_t request_metric;
};

struct operation_completion_chain_t
{
    void append (std::unique_ptr<operation_completion_item_t> completion,
                 operation_terminal_t terminal,
                 result_t<zlink::message_t> payload = result_t<zlink::message_t>::success ({}),
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

void operation_registry_state_t::reserve_deadline_visit (
  std::weak_ptr<operation_registry_state_t> self)
{
    // Registration, management and shutdown serialize this reservation on C2's lane.
    if (_deadline_visit)
        _deadline_visit->cancelled.store (true, std::memory_order_release);
    _deadline_visit.reset ();
    const auto at = next_deadline ();
    if (!at)
        return;
    auto visit = std::make_shared<detail::deadline_state_t> ();
    _deadline_visit = visit;
    detail::deadline_scheduler_t::instance ().schedule (*at, visit, [self, visit] {
        if (const auto active = self.lock ()) {
            // Serialize lane admission with close using the existing C1 mutex.
            // Once closed, a delayed scheduler callback cannot touch the borrowed lane.
            std::lock_guard lock (active->_mutex);
            if (!active->_closed)
                (void) active->_owner->try_post ([active, visit] {
                    if (active->_deadline_visit != visit)
                        return;
                    (void) active->expire (clock_t::now ());
                });
        }
    });
}

operation_registry_state_t::operation_registry_state_t (now_t now,
                                                        std::function<void ()> deadline_changed) :
    _drain_state (std::make_shared<operation_registry_drain_state_t> ()),
    _now (now),
    _deadline_origin (now ()),
    _deadline_changed (std::move (deadline_changed))
{
}

operation_registry_state_t::operation_registry_state_t (runtime::state_lane_t &owner,
                                                        now_t now,
                                                        std::function<void ()> deadline_changed) :
    _owner (&owner),
    _drain_state (std::make_shared<operation_registry_drain_state_t> ()),
    _now (now),
    _deadline_origin (now ()),
    _deadline_changed (std::move (deadline_changed))
{
}

operation_registry_state_t::~operation_registry_state_t () noexcept
{
    clear_deadlines ();
}

void operation_registry_state_t::clear_deadlines () noexcept
{
    for (auto *&head : _deadline_heads)
        while (head)
            unindex_deadline (*head);
    _entries.clear ();
    _next_visit.store (clock_t::duration::max ().count (), std::memory_order_release);
    _visit_slot = deadline_slot_count;
    _deadline_cursor = nullptr;
    if (_deadline_visit) {
        _deadline_visit->cancelled.store (true, std::memory_order_release);
        _deadline_visit.reset ();
        detail::deadline_scheduler_t::instance ().wake ();
    }
}


namespace
{
void notify (std::unique_ptr<operation_completion_item_t> completion,
             operation_terminal_t terminal,
             result_t<zlink::message_t> payload,
             std::optional<operation_terminal_t> request_terminal = {}) noexcept
{
    operation_completion_chain_t completions;
    completions.append (std::move (completion), terminal, std::move (payload), request_terminal);
    post_completions (std::move (completions));
}
}

bool operation_registry_state_t::register_operation (call_id_t id,
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
    pending_t prepared;
    prepared.target_routing_id = std::move (target_routing_id);
    prepared.request = std::move (request);
    prepared.id = id;
    prepared.deadline = deadline;
    prepared.completion = std::make_unique<operation_completion_item_t> (
      std::move (callback), _drain_state, detail::borrow_completion_dispatcher ());
    prepared.completion->request_metric = std::move (request_metric);
    if (cancellation.stop_possible ())
        prepared.completion->cancellation.emplace (cancellation,
                                                   operation_cancellation_t{this, id});
    bool earlier = false;
    bool accepted = true;
    std::exception_ptr publication_error;
    {
        std::lock_guard lock (_mutex);
        if (_closed || find_pending (_entries, id) != _entries.end ())
            return false;
        // Allocate the node and any buckets before transferring the stop
        // subscription. Allocation failure must not join cancellation under C1.
        auto position = _entries.emplace (id, pending_t{});
        position->second = std::move (prepared);
        auto *entry = &position->second;
        ++_pending_count;
        entry->completion->request_metric.start ();
        if (_owner && runtime::state_lane_t::current () != _owner) {
            try {
                accepted = _owner->try_post ([self = shared_from_this (), id, entry] {
                    bool earlier = false;
                    {
                        std::lock_guard lock (self->_mutex);
                        if (self->_closed)
                            return;
                        // Compare addresses without dereferencing a possibly retired
                        // capture. A close on the current lane may precede this turn.
                        const auto [begin, end] = self->_entries.equal_range (id);
                        const auto found = std::find_if (
                          begin, end, [entry] (const auto &item) { return &item.second == entry; });
                        if (found == end || found->second.deadline_slot != deadline_slot_count)
                            return;
                        earlier = self->index_deadline (id, found->second.deadline, found->second);
                    }
                    if (earlier && self->_deadline_changed)
                        self->_deadline_changed ();
                });
            }
            catch (const std::bad_alloc &) {
                publication_error = std::current_exception ();
                accepted = false;
            }
            if (!accepted) {
                prepared.completion = take_completion (position->second);
                _entries.erase (position);
            }
        } else {
            earlier = index_deadline (id, deadline, *entry);
        }
    }
    if (publication_error)
        std::rethrow_exception (publication_error);
    if (!accepted)
        return false;
    if (earlier && _deadline_changed)
        _deadline_changed ();
    // A cancellation between subscription and C1 publication saw no entry.
    if (cancellation.stop_requested ())
        cancel (id);
    return true;
}

std::optional<operation_registry_state_t::request_identity_t>
operation_registry_state_t::request_identity (call_id_t id, call_id_t operation) const
{
    std::lock_guard lock (_mutex);
    const auto found = find_pending (_entries, id);
    if (found == _entries.end () || !found->second.request
        || found->second.request->operation != operation)
        return std::nullopt;
    return found->second.request;
}

bool operation_registry_state_t::index_deadline (const call_id_t &id,
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
        _scan_minimum_deadline = std::min (_scan_minimum_deadline, deadline);
    return !reserved || next < *reserved;
}

void operation_registry_state_t::unindex_deadline (pending_t &pending) noexcept
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
    const auto [begin, end] = _entries.equal_range (pending.id);
    const auto found = std::find_if (
      begin, end, [&pending] (const auto &entry) { return &entry.second == &pending; });
    _entries.erase (found);
}

bool operation_registry_state_t::take (const call_id_t &id,
                                       std::unique_ptr<operation_completion_item_t> &completion)
{
    {
        std::lock_guard lock (_mutex);
        const auto found = find_pending (_entries, id);
        if (found == _entries.end ())
            return false;
        completion = take_completion (found->second);
    }
    completion->cancellation.reset ();
    return true;
}

bool operation_registry_state_t::complete (const call_id_t &id, result_t<zlink::message_t> payload)
{
    return complete (id, std::move (payload), {});
}

bool operation_registry_state_t::complete (const call_id_t &id,
                                           result_t<zlink::message_t> payload,
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
        payload = result_t<zlink::message_t>::success ({});
        notify (std::move (completion), operation_terminal_t::transport_failed,
                result_t<zlink::message_t>::success ({}));
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
            result_t<zlink::message_t>::success ({}));
}
} // namespace

bool operation_registry_state_t::complete_async (const call_id_t &id,
                                                 std::function<task_t<void> ()> before_dispatch)
{
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion))
        return false;
    // The existing completion reservation retains the drain until preparation ends.
    (void) finish_completion_async (std::move (completion), std::move (before_dispatch));
    return true;
}

bool operation_registry_state_t::cancel (const call_id_t &id)
{
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion)) {
        return false;
    }
    notify (std::move (completion), operation_terminal_t::cancelled,
            result_t<zlink::message_t>::success ({}));
    return true;
}

bool operation_registry_state_t::fail (const call_id_t &id,
                                       operation_terminal_t terminal,
                                       result_t<zlink::message_t> payload)
{
    return fail (id, terminal, std::move (payload), {});
}

bool operation_registry_state_t::fail (const call_id_t &id,
                                       operation_terminal_t terminal,
                                       result_t<zlink::message_t> payload,
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
        payload = result_t<zlink::message_t>::success ({});
    }
    notify (std::move (completion), terminal, std::move (payload), request_terminal);
    return true;
}

bool operation_registry_state_t::unregister (const call_id_t &id)
{
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion))
        return false;
    completion.reset ();
    return true;
}

bool operation_registry_state_t::contains (const call_id_t &id) const
{
    std::lock_guard lock (_mutex);
    return find_pending (_entries, id) != _entries.end ();
}

std::size_t
operation_registry_state_t::fail_target (const std::vector<std::uint8_t> &target_routing_id,
                                         operation_terminal_t terminal)
{
    if (target_routing_id.empty () || terminal == operation_terminal_t::completed)
        throw std::invalid_argument ("target failure requires a target and failure terminal");
    operation_completion_chain_t completions;
    {
        std::lock_guard lock (_mutex);
        for (auto entry = _entries.begin (); entry != _entries.end ();) {
            if (!entry->second.completion || entry->second.target_routing_id != target_routing_id) {
                ++entry;
                continue;
            }
            completions.append (take_completion (entry->second), terminal);
            ++entry;
        }
    }
    const auto failed = completions.size;
    post_completions (std::move (completions));
    return failed;
}

task_t<std::size_t> operation_registry_state_t::expire (clock_t::time_point now)
{
    if (!_owner || runtime::state_lane_t::current () == _owner)
        return task_t<std::size_t> (result_t<std::size_t>::success (expire_on_owner (now)));

    auto completion = std::make_shared<task_completion_source_t<std::size_t>> ();
    auto task = completion->task ();
    auto deliver = [completion, dispatcher = detail::borrow_completion_dispatcher ()] (
                     result_t<std::size_t> result) {
        detail::dispatch_completion (
          [completion, result = std::move (result)] () mutable {
              completion->complete (std::move (result));
          },
          false, completion.get (), dispatcher);
    };
    // C1 serializes admission with close, so a closed registry never touches
    // its borrowed lane. TryPost cannot run the turn inline under this mutex.
    std::lock_guard lock (_mutex);
    if (_closed
        || !_owner->try_post (
          [self = shared_from_this (), now, deliver] {
              try {
                  deliver (result_t<std::size_t>::success (self->expire_on_owner (now)));
              }
              catch (...) {
                  deliver (detail::current_exception_result<std::size_t> ());
              }
          },
          [deliver] (std::exception_ptr error) {
              deliver (detail::result_access_t::failure<std::size_t> (std::move (error)));
          })) {
        deliver (result_t<std::size_t>::failure (framework_error_kind_t::shutting_down,
                                                 "operation registry owner is closed"));
    }
    return task;
}

std::size_t operation_registry_state_t::expire_on_owner (clock_t::time_point now)
{
    operation_completion_chain_t completions;
    {
        std::unique_lock lock (_mutex);
        const auto visit = next_deadline ();
        if (!visit || now < *visit)
            return 0;
        const auto started = _now ();
        if (_visit_slot == deadline_slot_count) {
            _scan_minimum_deadline =
              now + deadline_tick_duration * static_cast<int> (deadline_slot_count);
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
                break;
            auto *current = _deadline_cursor;
            _deadline_cursor = current->deadline_next;
            std::unique_ptr<operation_completion_item_t> completion;
            if (current->completion && current->deadline <= now)
                completion = take_completion (*current);
            const bool live = bool (current->completion);
            if (completion)
                completions.append (std::move (completion), operation_terminal_t::timed_out);
            if (!live)
                unindex_deadline (*current);
            else
                _scan_minimum_deadline = std::min (_scan_minimum_deadline, current->deadline);
        }
        if (_visit_slot == deadline_slot_count) {
            _next_visit.store (_index_size ? _scan_minimum_deadline.time_since_epoch ().count ()
                                           : clock_t::duration::max ().count (),
                               std::memory_order_release);
        }
        const bool reschedule = _deadline_visit && _deadline_changed;
        lock.unlock ();
        if (reschedule)
            _deadline_changed ();
    }
    const auto expired = completions.size;
    post_completions (std::move (completions));
    return expired;
}

std::optional<operation_registry_state_t::clock_t::time_point>
operation_registry_state_t::next_deadline () const
{
    const auto ticks = _next_visit.load (std::memory_order_acquire);
    return ticks == clock_t::duration::max ().count ()
             ? std::nullopt
             : std::optional{clock_t::time_point{clock_t::duration{ticks}}};
}

std::size_t operation_registry_state_t::index_size () const
{
    return _index_size.load (std::memory_order_acquire);
}

std::size_t operation_registry_state_t::shutdown ()
{
    operation_completion_chain_t completions;
    {
        std::lock_guard lock (_mutex);
        if (!_closed) {
            _closed = true;
            for (auto &[id, pending] : _entries) {
                static_cast<void> (id);
                if (pending.completion)
                    completions.append (take_completion (pending), operation_terminal_t::shutdown);
            }
            // Entries also own storage for publications not yet indexed on the lane.
            if (!_entries.empty ()) {
                if (_owner && runtime::state_lane_t::current () != _owner) {
                    // FIFO follows every accepted index publication. The state, including
                    // retired entries, outlives the public registry until cleanup finishes.
                    (void) _owner->try_post ([self = shared_from_this ()] {
                        std::lock_guard lock (self->_mutex);
                        self->clear_deadlines ();
                    });
                    // A closing lane may reject cleanup while its final turn runs.
                    // The last state reference then clears storage in the destructor.
                } else {
                    clear_deadlines ();
                }
            }
        }
    }
    const auto stopped = completions.size;
    post_completions (std::move (completions));
    // A callback cannot wait for its own reservation. Other completion turns
    // still drain before registry shutdown returns.
    if (detail::current_completion_owner () != _drain_state.get ())
        _drain_state->wait ();
    return stopped;
}

std::size_t operation_registry_state_t::size () const
{
    std::lock_guard lock (_mutex);
    return _pending_count;
}

operation_registry_t::operation_registry_t () : operation_registry_t (&clock_t::now)
{
}
operation_registry_t::operation_registry_t (now_t now, std::function<void ()> changed) :
    _state (std::make_shared<operation_registry_state_t> (now, std::move (changed)))
{
}
operation_registry_t::operation_registry_t (runtime::state_lane_t &owner,
                                            now_t now,
                                            std::function<void ()> changed) :
    _state (std::make_shared<operation_registry_state_t> (owner, now, std::move (changed)))
{
}
operation_registry_t::~operation_registry_t () noexcept
{
    _state->shutdown ();
}
std::shared_ptr<operation_registry_t>
operation_registry_t::create_scheduled (runtime::state_lane_t &owner)
{
    auto registry = std::make_shared<operation_registry_t> (owner);
    const std::weak_ptr<operation_registry_state_t> weak = registry->_state;
    registry->_state->_deadline_changed = [weak] {
        if (const auto active = weak.lock ())
            active->reserve_deadline_visit (weak);
    };
    return registry;
}

std::optional<operation_registry_t::request_identity_t>
operation_registry_t::request_identity (call_id_t id, call_id_t operation) const
{
    return _state->request_identity (std::move (id), std::move (operation));
}

bool operation_registry_t::register_operation (call_id_t id,
                                               clock_t::time_point deadline,
                                               callback_t callback,
                                               std::vector<std::uint8_t> target_routing_id,
                                               mesh_request_metric_t request_metric,
                                               std::optional<request_identity_t> request,
                                               std::stop_token cancellation)
{
    return _state->register_operation (std::move (id), std::move (deadline), std::move (callback),
                                       std::move (target_routing_id), std::move (request_metric),
                                       std::move (request), std::move (cancellation));
}

bool operation_registry_t::contains (const call_id_t &id) const
{
    return _state->contains (std::move (id));
}

std::size_t operation_registry_t::fail_target (const std::vector<std::uint8_t> &target_routing_id,
                                               operation_terminal_t terminal)
{
    return _state->fail_target (std::move (target_routing_id), std::move (terminal));
}

bool operation_registry_t::complete (const call_id_t &id, result_t<zlink::message_t> payload)
{
    return _state->complete (std::move (id), std::move (payload));
}

bool operation_registry_t::complete (const call_id_t &id,
                                     result_t<zlink::message_t> payload,
                                     before_dispatch_t before_dispatch,
                                     operation_terminal_t request_terminal)
{
    return _state->complete (std::move (id), std::move (payload), std::move (before_dispatch),
                             std::move (request_terminal));
}

bool operation_registry_t::complete_async (const call_id_t &id,
                                           std::function<task_t<void> ()> before_dispatch)
{
    return _state->complete_async (std::move (id), std::move (before_dispatch));
}

bool operation_registry_t::cancel (const call_id_t &id)
{
    return _state->cancel (std::move (id));
}

bool operation_registry_t::fail (const call_id_t &id,
                                 operation_terminal_t terminal,
                                 result_t<zlink::message_t> payload)
{
    return _state->fail (std::move (id), std::move (terminal), std::move (payload));
}

bool operation_registry_t::fail (const call_id_t &id,
                                 operation_terminal_t terminal,
                                 result_t<zlink::message_t> payload,
                                 before_dispatch_t before_dispatch,
                                 std::optional<operation_terminal_t> request_terminal)
{
    return _state->fail (std::move (id), std::move (terminal), std::move (payload),
                         std::move (before_dispatch), std::move (request_terminal));
}

bool operation_registry_t::unregister (const call_id_t &id)
{
    return _state->unregister (std::move (id));
}

task_t<std::size_t> operation_registry_t::expire (clock_t::time_point now)
{
    return _state->expire (std::move (now));
}

std::optional<operation_registry_t::clock_t::time_point>
operation_registry_t::next_deadline () const
{
    return _state->next_deadline ();
}

std::size_t operation_registry_t::shutdown ()
{
    // A delivered callback may release the public registry while shutdown drains.
    const auto state = _state;
    return state->shutdown ();
}

std::size_t operation_registry_t::size () const
{
    return _state->size ();
}

std::size_t operation_registry_t::index_size () const
{
    return _state->index_size ();
}

} // namespace zlink::framework::runtime::foundation
