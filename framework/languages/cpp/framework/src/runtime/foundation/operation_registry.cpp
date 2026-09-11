/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/foundation/operation_registry.hpp"
#include "runtime/diagnostics/mesh_request_metrics.hpp"

#include <condition_variable>
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
        const auto outcome = terminal == operation_terminal_t::completed ? "completed"
                           : terminal == operation_terminal_t::timed_out ? "timed_out"
                           : terminal == operation_terminal_t::cancelled ? "cancelled"
                           : terminal == operation_terminal_t::shutdown ? "shutdown"
                           : "failed";
        request_metric.complete (outcome);
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

    std::unique_ptr<operation_completion_item_t> try_admit (
      operation_registry_t::callback_t callback,
      const std::shared_ptr<operation_registry_drain_state_t> &drain_state)
    {
        {
            std::lock_guard lock (_state->mutex);
            if (_state->reserved >= default_operation_capacity)
                return {};
            ++_state->reserved;
        }
        try {
            return std::make_unique<operation_completion_item_t> (
              std::move (callback), drain_state);
        }
        catch (...) {
            release_reservation (_state);
            throw;
        }
    }

    void post (std::unique_ptr<operation_completion_item_t> completion,
               operation_terminal_t terminal,
               std::vector<std::uint8_t> payload,
               std::optional<operation_terminal_t> request_terminal = {}) noexcept
    {
        operation_completion_chain_t completions;
        completions.append (
          std::move (completion), terminal, std::move (payload), request_terminal);
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
        release_reservation (_state);
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
        std::size_t reserved = 0;
        bool stopping = false;
    };

    static void release_reservation (
      const std::shared_ptr<state_t> &state) noexcept
    {
        std::lock_guard lock (state->mutex);
        if (state->reserved != 0)
            --state->reserved;
    }

    static void run (std::shared_ptr<state_t> state) noexcept
    {
        completion_dispatcher_current = true;
        for (;;) {
            std::unique_ptr<operation_completion_item_t> completion;
            {
                std::unique_lock lock (state->mutex);
                state->ready.wait (lock, [&] {
                    return state->stopping || state->head;
                });
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
                completion->callback (
                  completion->terminal,
                  std::move (completion->payload));
            }
            catch (...) {
                // One consumer must not prevent later terminal callbacks
                // from running on the process-shared dispatcher lane.
            }
            release_reservation (state);
            completion->release_owner ();
            completion.reset ();
        }
    }

    std::shared_ptr<state_t> _state;
    std::thread _worker;
};

namespace
{

std::shared_ptr<operation_completion_dispatcher_t>
shared_completion_dispatcher ()
{
    // One process-lifetime dispatcher keeps both the worker count and the
    // aggregate pending-plus-queued reservation bound stable while owners are
    // stopped and recreated. A weak singleton could temporarily run an old
    // draining lane beside a newly created lane and multiply that bound.
    static auto dispatcher =
      std::make_shared<operation_completion_dispatcher_t> ();
    return dispatcher;
}

} // namespace

operation_registry_t::operation_registry_t (std::size_t capacity) :
    _capacity (capacity),
    _completion_dispatcher (shared_completion_dispatcher ()),
    _drain_state (std::make_shared<operation_registry_drain_state_t> ())
{
    if (capacity == 0) {
        throw std::invalid_argument ("operation registry capacity must be positive");
    }
}

operation_registry_t::~operation_registry_t () noexcept
{
    shutdown ();
}

namespace
{
void notify (
             const std::shared_ptr<operation_completion_dispatcher_t> &dispatcher,
             std::unique_ptr<operation_completion_item_t> completion,
             operation_terminal_t terminal,
             std::vector<std::uint8_t> payload,
             std::optional<operation_terminal_t> request_terminal = {}) noexcept
{
    dispatcher->post (
      std::move (completion), terminal, std::move (payload), request_terminal);
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
    if (_closed || _pending.size () >= _capacity
        || _pending.contains (id)) {
        return false;
    }
    const auto inserted = _pending.emplace (
      id, pending_t{deadline, {}, std::move (target_routing_id)});
    if (!inserted.second)
        return false;
    try {
        auto completion = _completion_dispatcher->try_admit (
          std::move (callback), _drain_state);
        if (!completion) {
            _pending.erase (inserted.first);
            return false;
        }
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

bool operation_registry_t::take (
  const call_id_t &id,
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

bool operation_registry_t::complete (const call_id_t &id,
                                     std::vector<std::uint8_t> payload)
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
    notify (_completion_dispatcher, std::move (completion),
            operation_terminal_t::completed, std::move (payload), request_terminal);
    return true;
}

bool operation_registry_t::cancel (const call_id_t &id)
{
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion)) {
        return false;
    }
    notify (_completion_dispatcher, std::move (completion),
            operation_terminal_t::cancelled, {});
    return true;
}

bool operation_registry_t::fail (
  const call_id_t &id,
  operation_terminal_t terminal,
  std::vector<std::uint8_t> payload)
{
    return fail (id, terminal, std::move (payload), {});
}

bool operation_registry_t::fail (
  const call_id_t &id,
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
    notify (_completion_dispatcher, std::move (completion), terminal,
            std::move (payload), request_terminal);
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

std::size_t operation_registry_t::fail_target (
  const std::vector<std::uint8_t> &target_routing_id,
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
    const auto failed = completions.size;
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
            completions.append (
              std::move (entry->second.completion),
              operation_terminal_t::timed_out);
            entry = _pending.erase (entry);
        }
    }
    const auto expired = completions.size;
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
                completions.append (
                  std::move (pending.completion),
                  operation_terminal_t::shutdown);
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
