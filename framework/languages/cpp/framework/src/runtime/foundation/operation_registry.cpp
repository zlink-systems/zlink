/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/foundation/operation_registry.hpp"

#include <condition_variable>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zlink::framework::runtime::foundation
{

struct operation_completion_item_t
{
    explicit operation_completion_item_t (
      operation_registry_t::callback_t callback) :
        callback (std::move (callback))
    {
    }

    operation_registry_t::callback_t callback;
    operation_terminal_t terminal = operation_terminal_t::transport_failed;
    std::vector<std::uint8_t> payload;
    std::unique_ptr<operation_completion_item_t> next;
};

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
      operation_registry_t::callback_t callback)
    {
        {
            std::lock_guard lock (_state->mutex);
            if (_state->reserved >= default_operation_capacity)
                return {};
            ++_state->reserved;
        }
        try {
            return std::make_unique<operation_completion_item_t> (
              std::move (callback));
        }
        catch (...) {
            release_reservation (_state);
            throw;
        }
    }

    void post (std::unique_ptr<operation_completion_item_t> completion,
               operation_terminal_t terminal,
               std::vector<std::uint8_t> payload) noexcept
    {
        completion->terminal = terminal;
        completion->payload = std::move (payload);
        auto *tail = completion.get ();
        {
            std::lock_guard lock (_state->mutex);
            if (_state->tail)
                _state->tail->next = std::move (completion);
            else
                _state->head = std::move (completion);
            _state->tail = tail;
        }
        _state->ready.notify_one ();
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
            completion.reset ();
            release_reservation (state);
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
    _completion_dispatcher (shared_completion_dispatcher ())
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
             std::vector<std::uint8_t> payload) noexcept
{
    dispatcher->post (
      std::move (completion), terminal, std::move (payload));
}
}

bool operation_registry_t::register_operation (call_id_t id,
                                               clock_t::time_point deadline,
                                               callback_t callback)
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
      id, pending_t{deadline, {}});
    if (!inserted.second)
        return false;
    try {
        auto completion = _completion_dispatcher->try_admit (
          std::move (callback));
        if (!completion) {
            _pending.erase (inserted.first);
            return false;
        }
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
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion)) {
        return false;
    }
    notify (_completion_dispatcher, std::move (completion),
            operation_terminal_t::completed, std::move (payload));
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
    if (terminal == operation_terminal_t::completed) {
        throw std::invalid_argument ("failure terminal cannot be completed");
    }
    std::unique_ptr<operation_completion_item_t> completion;
    if (!take (id, completion)) {
        return false;
    }
    notify (_completion_dispatcher, std::move (completion), terminal,
            std::move (payload));
    return true;
}

std::size_t operation_registry_t::expire (clock_t::time_point now)
{
    std::vector<std::unique_ptr<operation_completion_item_t>> completions;
    {
        std::lock_guard lock (_mutex);
        completions.reserve (_pending.size ());
        for (auto entry = _pending.begin (); entry != _pending.end ();) {
            if (entry->second.deadline > now) {
                ++entry;
                continue;
            }
            completions.push_back (
              std::move (entry->second.completion));
            entry = _pending.erase (entry);
        }
    }
    for (auto &completion : completions) {
        notify (_completion_dispatcher, std::move (completion),
                operation_terminal_t::timed_out, {});
    }
    return completions.size ();
}

std::size_t operation_registry_t::shutdown ()
{
    std::vector<std::unique_ptr<operation_completion_item_t>> completions;
    {
        std::lock_guard lock (_mutex);
        if (_closed) {
            return 0;
        }
        _closed = true;
        completions.reserve (_pending.size ());
        for (auto &[id, pending] : _pending) {
            static_cast<void> (id);
            completions.push_back (
              std::move (pending.completion));
        }
        _pending.clear ();
    }
    for (auto &completion : completions) {
        notify (_completion_dispatcher, std::move (completion),
                operation_terminal_t::shutdown, {});
    }
    return completions.size ();
}

std::size_t operation_registry_t::size () const
{
    std::lock_guard lock (_mutex);
    return _pending.size ();
}

} // namespace zlink::framework::runtime::foundation
