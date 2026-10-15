/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/foundation/operation_registry.hpp"
#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"

#include <algorithm>
#include <condition_variable>
#include <bit>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zlink::framework::runtime::foundation
{

struct operation_registry_drain_state_t
{
    void admit () noexcept
    {
        std::lock_guard lock (mutex);
        ++outstanding;
    }

    void release () noexcept
    {
        {
            std::lock_guard lock (mutex);
            if (outstanding != 0)
                --outstanding;
        }
        drained.notify_all ();
    }

    void wait () noexcept
    {
        std::unique_lock lock (mutex);
        drained.wait (lock, [&] { return outstanding == 0; });
    }

    std::mutex mutex;
    std::condition_variable drained;
    std::size_t outstanding = 0;
};

struct operation_completion_item_t
{
    explicit operation_completion_item_t (
      operation_registry_t::callback_t callback,
      std::shared_ptr<operation_registry_drain_state_t> drain_state) :
        callback (std::move (callback)), drain_state (std::move (drain_state))
    {
        this->drain_state->admit ();
    }

    ~operation_completion_item_t ()
    {
        if (drain_state)
            drain_state->release ();
    }

    void release_owner () noexcept
    {
        auto owner = std::move (drain_state);
        if (owner)
            owner->release ();
    }

    void finish_metrics (operation_terminal_t terminal) noexcept
    {
        request_metric.complete (terminal);
    }

    operation_registry_t::callback_t callback;
    std::shared_ptr<operation_registry_drain_state_t> drain_state;
    operation_terminal_t terminal = operation_terminal_t::transport_failed;
    std::vector<std::uint8_t> payload;
    std::unique_ptr<operation_completion_item_t> next;
    mesh_request_metric_t request_metric;
};

struct operation_completion_chain_t
{
    void append (std::unique_ptr<operation_completion_item_t> completion,
                 operation_terminal_t terminal,
                 std::vector<std::uint8_t> payload = {},
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

thread_local bool completion_dispatcher_current = false;

bool is_on_completion_dispatcher () noexcept
{
    return completion_dispatcher_current;
}

class operation_completion_dispatcher_t
{
  public:
    operation_completion_dispatcher_t () :
        _state (std::make_shared<state_t> ()),
        _worker ([state = _state] { run (std::move (state)); })
    {
    }

    ~operation_completion_dispatcher_t () noexcept
    {
        {
            std::lock_guard lock (_state->mutex);
            _state->stopping = true;
        }
        _state->ready.notify_all ();
        if (!_worker.joinable ())
            return;
        if (_worker.get_id () == std::this_thread::get_id ()) {
            // A completion may release the final owner. The worker only uses
            // the shared state, so it can finish the current turn and drain
            // the remaining callbacks after this dispatcher object is gone.
            _worker.detach ();
            return;
        }
        _worker.join ();
    }

    std::unique_ptr<operation_completion_item_t>
    create_completion_item (operation_registry_t::callback_t callback,
                            const std::shared_ptr<operation_registry_drain_state_t> &drain_state)
    {
        return std::make_unique<operation_completion_item_t> (std::move (callback), drain_state);
    }

    void post (std::unique_ptr<operation_completion_item_t> completion,
               operation_terminal_t terminal,
               std::vector<std::uint8_t> payload,
               std::optional<operation_terminal_t> request_terminal = {}) noexcept
    {
        operation_completion_chain_t completions;
        completions.append (std::move (completion), terminal, std::move (payload),
                            request_terminal);
        post_chain (std::move (completions));
    }

    void post_chain (operation_completion_chain_t completions) noexcept
    {
        if (!completions.head)
            return;
        {
            std::lock_guard lock (_state->mutex);
            if (_state->tail)
                _state->tail->next = std::move (completions.head);
            else
                _state->head = std::move (completions.head);
            _state->tail = completions.tail;
        }
        _state->ready.notify_one ();
    }

    void discard (std::unique_ptr<operation_completion_item_t> completion) noexcept
    {
        if (!completion)
            return;
        completion->release_owner ();
        completion.reset ();
    }

  private:
    struct state_t
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::unique_ptr<operation_completion_item_t> head;
        operation_completion_item_t *tail = nullptr;
        bool stopping = false;
    };

    static void run (std::shared_ptr<state_t> state) noexcept
    {
        completion_dispatcher_current = true;
        for (;;) {
            std::unique_ptr<operation_completion_item_t> completion;
            {
                std::unique_lock lock (state->mutex);
                state->ready.wait (lock, [&] { return state->stopping || state->head; });
                if (!state->head) {
                    if (state->stopping)
                        return;
                    continue;
                }
                completion = std::move (state->head);
                state->head = std::move (completion->next);
                if (!state->head)
                    state->tail = nullptr;
            }
            try {
                completion->callback (completion->terminal, std::move (completion->payload));
            }
            catch (...) {
                // One consumer must not prevent later terminal callbacks
                // from running on the process-shared dispatcher lane.
            }
            completion->release_owner ();
            completion.reset ();
        }
    }

    std::shared_ptr<state_t> _state;
    std::thread _worker;
};

namespace
{

std::shared_ptr<operation_completion_dispatcher_t> shared_completion_dispatcher ()
{
    // One process-lifetime dispatcher prevents owners that are stopped and
    // recreated from temporarily running separate completion lanes.
    static auto dispatcher = std::make_shared<operation_completion_dispatcher_t> ();
    return dispatcher;
}

} // namespace

operation_registry_t::operation_registry_t () :
    _completion_dispatcher (shared_completion_dispatcher ()),
    _drain_state (std::make_shared<operation_registry_drain_state_t> ())
{
}

operation_registry_t::~operation_registry_t () noexcept
{
    shutdown ();
}

namespace
{
void notify (const std::shared_ptr<operation_completion_dispatcher_t> &dispatcher,
             std::unique_ptr<operation_completion_item_t> completion,
             operation_terminal_t terminal,
             std::vector<std::uint8_t> payload,
             std::optional<operation_terminal_t> request_terminal = {}) noexcept
{
    dispatcher->post (std::move (completion), terminal, std::move (payload), request_terminal);
}
}

bool operation_registry_t::register_operation (call_id_t id,
                                               clock_t::time_point deadline,
                                               callback_t callback,
                                               std::vector<std::uint8_t> target_routing_id,
                                               mesh_request_metric_t request_metric)
{
    if (!callback) {
        throw std::invalid_argument ("operation callback is required");
    }
    if (id.high == 0 && id.low == 0) {
        throw std::invalid_argument ("operation id must not be zero");
    }
    std::lock_guard lock (_mutex);
    if (_closed || _pending.contains (id)) {
        return false;
    }
    const auto inserted = _pending.emplace (id, pending_t{{}, std::move (target_routing_id), {}});
    if (!inserted.second)
        return false;
    try {
        index_deadline (id, deadline, inserted.first->second);
        auto completion =
          _completion_dispatcher->create_completion_item (std::move (callback), _drain_state);
        completion->request_metric = std::move (request_metric);
        completion->request_metric.start ();
        inserted.first->second.completion = std::move (completion);
        return true;
    }
    catch (...) {
        unindex_deadline (inserted.first->second);
        _pending.erase (inserted.first);
        throw;
    }
}

std::optional<std::uint64_t> operation_registry_t::next_occupied_tick () const noexcept
{
    const auto slot = static_cast<std::size_t> (_deadline_tick & (deadline_slot_count - 1));
    const auto first_word = slot / deadline_bitmap_word_bits;
    const auto words = _deadline_occupied.size ();
    for (std::size_t step = 0; step <= words; ++step) {
        const auto word = (first_word + step) % words;
        auto bits = _deadline_occupied[word];
        const auto mask = ~std::uint64_t{0} << (slot % deadline_bitmap_word_bits);
        if (step == 0)
            bits &= mask;
        if (step == words)
            bits &= ~mask;
        if (bits != 0) {
            const auto next = word * deadline_bitmap_word_bits + std::countr_zero (bits);
            return _deadline_tick + ((next - slot) & (deadline_slot_count - 1));
        }
    }
    return {};
}

void operation_registry_t::index_deadline (const call_id_t &id,
                                           clock_t::time_point deadline,
                                           pending_t &pending)
{
    const auto relative =
      deadline <= _deadline_origin
        ? std::chrono::milliseconds::zero ()
        : std::chrono::ceil<std::chrono::milliseconds> (deadline - _deadline_origin);
    const auto tick = std::max (_deadline_tick, static_cast<std::uint64_t> (relative.count ()));
    const auto slot = static_cast<std::size_t> (tick & (deadline_slot_count - 1));
    pending.id = id;
    pending.deadline_slot = slot;
    pending.deadline_rounds = (tick - _deadline_tick) / deadline_slot_count;
    pending.deadline_previous = _deadline_tails[slot];
    if (pending.deadline_previous)
        pending.deadline_previous->deadline_next = &pending;
    else
        _deadline_heads[slot] = &pending;
    _deadline_tails[slot] = &pending;
    _deadline_occupied[slot / deadline_bitmap_word_bits] |= std::uint64_t{1}
                                                            << (slot % deadline_bitmap_word_bits);
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
    if (_deadline_cursor == &pending) {
        _deadline_cursor = next;
        if (!next)
            ++_deadline_tick;
    }
    if (!_deadline_heads[slot])
        _deadline_occupied[slot / deadline_bitmap_word_bits] &=
          ~(std::uint64_t{1} << (slot % deadline_bitmap_word_bits));
    pending.deadline_previous = nullptr;
    pending.deadline_next = nullptr;
}

bool operation_registry_t::take (const call_id_t &id,
                                 std::unique_ptr<operation_completion_item_t> &completion)
{
    std::lock_guard lock (_mutex);
    const auto found = _pending.find (id);
    if (found == _pending.end ()) {
        return false;
    }
    completion = std::move (found->second.completion);
    unindex_deadline (found->second);
    _pending.erase (found);
    return true;
}

bool operation_registry_t::complete (const call_id_t &id, std::vector<std::uint8_t> payload)
{
    return complete (id, std::move (payload), {});
}

bool operation_registry_t::complete (const call_id_t &id,
                                     std::vector<std::uint8_t> payload,
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
        payload.clear ();
        notify (_completion_dispatcher, std::move (completion),
                operation_terminal_t::transport_failed, {});
        return true;
    }
    notify (_completion_dispatcher, std::move (completion), operation_terminal_t::completed,
            std::move (payload), request_terminal);
    return true;
}

bool operation_registry_t::cancel (const call_id_t &id)
{
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion)) {
        return false;
    }
    notify (_completion_dispatcher, std::move (completion), operation_terminal_t::cancelled, {});
    return true;
}

bool operation_registry_t::fail (const call_id_t &id,
                                 operation_terminal_t terminal,
                                 std::vector<std::uint8_t> payload)
{
    return fail (id, terminal, std::move (payload), {});
}

bool operation_registry_t::fail (const call_id_t &id,
                                 operation_terminal_t terminal,
                                 std::vector<std::uint8_t> payload,
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
        payload.clear ();
    }
    notify (_completion_dispatcher, std::move (completion), terminal, std::move (payload),
            request_terminal);
    return true;
}

bool operation_registry_t::unregister (const call_id_t &id)
{
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion))
        return false;
    _completion_dispatcher->discard (std::move (completion));
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
            if (entry->second.target_routing_id != target_routing_id) {
                ++entry;
                continue;
            }
            completions.append (std::move (entry->second.completion), terminal);
            unindex_deadline (entry->second);
            entry = _pending.erase (entry);
        }
    }
    const auto failed = completions.size;
    _completion_dispatcher->post_chain (std::move (completions));
    return failed;
}

std::size_t operation_registry_t::expire (clock_t::time_point now)
{
    operation_completion_chain_t completions;
    clock_t::time_point started;
    const auto now_tick =
      now <= _deadline_origin
        ? std::uint64_t{0}
        : static_cast<std::uint64_t> (
            std::chrono::duration_cast<std::chrono::milliseconds> (now - _deadline_origin)
              .count ());
    while (completions.size == 0
           || clock_t::now () - started < runtime::dispatch_limits::receive_batch_time) {
        std::lock_guard lock (_mutex);
        if (!_deadline_cursor) {
            const auto next = next_occupied_tick ();
            if (!next) {
                _deadline_tick = std::max (_deadline_tick, now_tick + 1);
                break;
            }
            if (*next > now_tick)
                break;
            _deadline_tick = *next;
            _deadline_cursor = _deadline_heads[*next & (deadline_slot_count - 1)];
        }
        auto *current = _deadline_cursor;
        if (current->deadline_rounds > 0) {
            --current->deadline_rounds;
            _deadline_cursor = current->deadline_next;
            if (!_deadline_cursor)
                ++_deadline_tick;
            continue;
        }
        if (completions.size == 0)
            started = clock_t::now ();
        const auto entry = _pending.find (current->id);
        completions.append (std::move (entry->second.completion), operation_terminal_t::timed_out);
        unindex_deadline (entry->second);
        _pending.erase (entry);
    }
    const auto expired = completions.size;
    _completion_dispatcher->post_chain (std::move (completions));
    return expired;
}

std::optional<operation_registry_t::clock_t::time_point>
operation_registry_t::next_deadline () const
{
    std::lock_guard lock (_mutex);
    const auto tick = next_occupied_tick ();
    if (!tick)
        return {};
    return _deadline_origin + deadline_tick_duration * *tick;
}

std::size_t operation_registry_t::shutdown ()
{
    operation_completion_chain_t completions;
    std::size_t stopped = 0;
    {
        std::lock_guard lock (_mutex);
        if (!_closed) {
            _closed = true;
            for (auto &[id, pending] : _pending) {
                static_cast<void> (id);
                completions.append (std::move (pending.completion), operation_terminal_t::shutdown);
                unindex_deadline (pending);
            }
            _pending.clear ();
            stopped = completions.size;
        }
    }
    _completion_dispatcher->post_chain (std::move (completions));
    // A callback can release the final owner and re-enter shutdown on the
    // dispatcher itself. Its item owns the drain state, so that path may
    // return and let the worker release the final reservation naturally.
    if (!completion_dispatcher_current)
        _drain_state->wait ();
    return stopped;
}

std::size_t operation_registry_t::size () const
{
    std::lock_guard lock (_mutex);
    return _pending.size ();
}

} // namespace zlink::framework::runtime::foundation
