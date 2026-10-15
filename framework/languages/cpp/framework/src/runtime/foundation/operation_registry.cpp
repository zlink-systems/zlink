/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/foundation/operation_registry.hpp"
#include "runtime/diagnostics/mesh_request_metrics.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
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
    zlink::message_t payload;
    std::unique_ptr<operation_completion_item_t> next;
    mesh_request_metric_t request_metric;
};

operation_completion_chain_t::operation_completion_chain_t () noexcept = default;

operation_completion_chain_t::~operation_completion_chain_t ()
{
    while (head)
        head = std::move (head->next);
}

operation_completion_chain_t::operation_completion_chain_t (
  operation_completion_chain_t &&other) noexcept :
    head (std::move (other.head)),
    tail (std::exchange (other.tail, nullptr)),
    _size (std::exchange (other._size, 0))
{
}

void operation_completion_chain_t::append (
  std::unique_ptr<operation_completion_item_t> completion,
  operation_terminal_t terminal,
  zlink::message_t payload,
  std::optional<operation_terminal_t> request_terminal) noexcept
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
    ++_size;
}

void operation_completion_chain_t::append (operation_completion_chain_t completions) noexcept
{
    if (!completions.head)
        return;
    if (tail)
        tail->next = std::move (completions.head);
    else
        head = std::move (completions.head);
    tail = completions.tail;
    _size += completions._size;
}

operation_completion_chain_t operation_completion_chain_t::take_first () noexcept
{
    operation_completion_chain_t first;
    if (!head)
        return first;
    first.head = std::move (head);
    head = std::move (first.head->next);
    first.tail = first.head.get ();
    first._size = 1;
    if (--_size == 0)
        tail = nullptr;
    return first;
}

thread_local operation_completion_chain_t *completion_collector_current = nullptr;

operation_completion_collector_t::operation_completion_collector_t (
  operation_completion_chain_t &completions) noexcept :
    _previous (std::exchange (completion_collector_current, &completions))
{
}

operation_completion_collector_t::~operation_completion_collector_t ()
{
    completion_collector_current = _previous;
}

thread_local bool completion_dispatcher_current = false;

bool is_on_completion_dispatcher () noexcept
{
    return completion_dispatcher_current;
}

namespace
{
void run_completion (std::unique_ptr<operation_completion_item_t> completion) noexcept
{
    const auto previous = std::exchange (completion_dispatcher_current, true);
    try {
        completion->callback (completion->terminal, std::move (completion->payload));
    }
    catch (...) {
        // One consumer must not prevent later terminal callbacks from running.
    }
    completion->release_owner ();
    completion.reset ();
    completion_dispatcher_current = previous;
}
} // namespace

void operation_completion_chain_t::run_one () noexcept
{
    assert (_size == 1 && head && !head->next);
    auto completion = std::move (head);
    tail = nullptr;
    _size = 0;
    run_completion (std::move (completion));
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
        _state->pending.fetch_or (stopping, std::memory_order_release);
        _state->pending.notify_all ();
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
               zlink::message_t payload,
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
        if (completion_collector_current) {
            completion_collector_current->append (std::move (completions));
            return;
        }
        auto *tail = completions.head.get ();
        completions.head = reverse (std::move (completions.head));
        auto current = _state->pending.load (std::memory_order_relaxed);
        for (;;) {
            tail->next.reset (item (current));
            const auto next =
              reinterpret_cast<std::uintptr_t> (completions.head.get ()) | (current & stopping);
            if (_state->pending.compare_exchange_weak (current, next, std::memory_order_release,
                                                       std::memory_order_relaxed)) {
                completions.head.release ();
                if (item (current) == nullptr)
                    _state->pending.notify_one ();
                return;
            }
            // A failed publication never owns the observed queue prefix.
            tail->next.release ();
        }
    }

    void discard (std::unique_ptr<operation_completion_item_t> completion) noexcept
    {
        if (!completion)
            return;
        completion->release_owner ();
        completion.reset ();
    }

  private:
    static constexpr std::uintptr_t stopping = 1;
    static_assert (alignof (operation_completion_item_t) > stopping);
    struct state_t
    {
        // Completions published after the worker's final drain still own
        // their registry drain slots; release them with the shared state.
        ~state_t ()
        {
            std::unique_ptr<operation_completion_item_t> head (
              item (pending.load (std::memory_order_acquire)));
            while (head)
                head = std::move (head->next);
        }

        std::atomic<std::uintptr_t> pending{0};
    };

    static operation_completion_item_t *item (std::uintptr_t pending) noexcept
    {
        return reinterpret_cast<operation_completion_item_t *> (pending & ~stopping);
    }

    static std::unique_ptr<operation_completion_item_t>
    reverse (std::unique_ptr<operation_completion_item_t> head) noexcept
    {
        std::unique_ptr<operation_completion_item_t> reversed;
        while (head) {
            auto next = std::move (head->next);
            head->next = std::move (reversed);
            reversed = std::move (head);
            head = std::move (next);
        }
        return reversed;
    }

    static void run (std::shared_ptr<state_t> state) noexcept
    {
        completion_dispatcher_current = true;
        for (;;) {
            auto pending = state->pending.load (std::memory_order_acquire);
            if (pending == 0) {
                state->pending.wait (0, std::memory_order_acquire);
                continue;
            }
            pending = state->pending.fetch_and (stopping, std::memory_order_acq_rel);
            auto ready = reverse (std::unique_ptr<operation_completion_item_t> (item (pending)));
            if (!ready && (pending & stopping) != 0)
                return;
            while (ready) {
                auto completion = std::move (ready);
                ready = std::move (completion->next);
                run_completion (std::move (completion));
            }
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

void operation_completion_chain_t::publish () noexcept
{
    if (head)
        shared_completion_dispatcher ()->post_chain (std::move (*this));
}

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
             zlink::message_t payload,
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
    const auto inserted =
      _pending.emplace (id, pending_t{deadline, {}, std::move (target_routing_id)});
    if (!inserted.second)
        return false;
    try {
        auto completion =
          _completion_dispatcher->create_completion_item (std::move (callback), _drain_state);
        completion->request_metric = std::move (request_metric);
        completion->request_metric.start ();
        inserted.first->second.completion = std::move (completion);
        return true;
    }
    catch (...) {
        _pending.erase (inserted.first);
        throw;
    }
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
    _pending.erase (found);
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
            entry = _pending.erase (entry);
        }
    }
    const auto failed = completions.size ();
    _completion_dispatcher->post_chain (std::move (completions));
    return failed;
}

std::size_t operation_registry_t::expire (clock_t::time_point now)
{
    operation_completion_chain_t completions;
    {
        std::lock_guard lock (_mutex);
        for (auto entry = _pending.begin (); entry != _pending.end ();) {
            if (entry->second.deadline > now) {
                ++entry;
                continue;
            }
            completions.append (std::move (entry->second.completion),
                                operation_terminal_t::timed_out);
            entry = _pending.erase (entry);
        }
    }
    const auto expired = completions.size ();
    _completion_dispatcher->post_chain (std::move (completions));
    return expired;
}

std::optional<operation_registry_t::clock_t::time_point>
operation_registry_t::next_deadline () const
{
    std::lock_guard lock (_mutex);
    std::optional<clock_t::time_point> result;
    for (const auto &[_, pending] : _pending) {
        if (!result || pending.deadline < *result)
            result = pending.deadline;
    }
    return result;
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
            }
            _pending.clear ();
            stopped = completions.size ();
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
