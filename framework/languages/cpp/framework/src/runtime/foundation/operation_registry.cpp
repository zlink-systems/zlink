/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/foundation/operation_registry.hpp"

#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::foundation
{

operation_registry_t::operation_registry_t (std::size_t capacity) : _capacity (capacity)
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
void notify (operation_registry_t::callback_t &callback,
             operation_terminal_t terminal,
             std::vector<std::uint8_t> payload) noexcept
{
    try {
        callback (terminal, std::move (payload));
    } catch (...) {
        // Completion callbacks are isolated so one consumer cannot prevent
        // terminal notification and cleanup for the remaining operations.
    }
}
}

bool operation_registry_t::register_operation (operation_id_t id,
                                               clock_t::time_point deadline,
                                               callback_t callback)
{
    if (!callback) {
        throw std::invalid_argument ("operation callback is required");
    }
    std::lock_guard lock (_mutex);
    if (_closed || _pending.size () >= _capacity) {
        return false;
    }
    return _pending.emplace (std::move (id), pending_t{deadline, std::move (callback)}).second;
}

bool operation_registry_t::take (const operation_id_t &id, callback_t &callback)
{
    std::lock_guard lock (_mutex);
    const auto found = _pending.find (id);
    if (found == _pending.end ()) {
        return false;
    }
    callback = std::move (found->second.callback);
    _pending.erase (found);
    return true;
}

bool operation_registry_t::complete (const operation_id_t &id,
                                     std::vector<std::uint8_t> payload)
{
    callback_t callback;
    if (!take (id, callback)) {
        return false;
    }
    notify (callback, operation_terminal_t::completed, std::move (payload));
    return true;
}

bool operation_registry_t::cancel (const operation_id_t &id)
{
    callback_t callback;
    if (!take (id, callback)) {
        return false;
    }
    notify (callback, operation_terminal_t::cancelled, {});
    return true;
}

bool operation_registry_t::fail (
  const operation_id_t &id,
  operation_terminal_t terminal)
{
    if (terminal == operation_terminal_t::completed) {
        throw std::invalid_argument ("failure terminal cannot be completed");
    }
    callback_t callback;
    if (!take (id, callback)) {
        return false;
    }
    notify (callback, terminal, {});
    return true;
}

std::size_t operation_registry_t::expire (clock_t::time_point now)
{
    std::vector<callback_t> callbacks;
    {
        std::lock_guard lock (_mutex);
        for (auto entry = _pending.begin (); entry != _pending.end ();) {
            if (entry->second.deadline > now) {
                ++entry;
                continue;
            }
            callbacks.push_back (std::move (entry->second.callback));
            entry = _pending.erase (entry);
        }
    }
    for (auto &callback : callbacks) {
        notify (callback, operation_terminal_t::timed_out, {});
    }
    return callbacks.size ();
}

std::size_t operation_registry_t::shutdown ()
{
    std::vector<callback_t> callbacks;
    {
        std::lock_guard lock (_mutex);
        if (_closed) {
            return 0;
        }
        _closed = true;
        callbacks.reserve (_pending.size ());
        for (auto &[id, pending] : _pending) {
            static_cast<void> (id);
            callbacks.push_back (std::move (pending.callback));
        }
        _pending.clear ();
    }
    for (auto &callback : callbacks) {
        notify (callback, operation_terminal_t::shutdown, {});
    }
    return callbacks.size ();
}

std::size_t operation_registry_t::size () const
{
    std::lock_guard lock (_mutex);
    return _pending.size ();
}

} // namespace zlink::framework::runtime::foundation
