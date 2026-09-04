/* SPDX-License-Identifier: MPL-2.0 */
#include "completion_owner.hpp"

#include "operation_state.hpp"
#include "operation_submit.hpp"
#include <Runtime/Native/message_access.hpp>

#include <zlink.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <system_error>

namespace zlink::detail
{
namespace
{

class completion_guard_t
{
  public:
    explicit completion_guard_t (zlink_completion_t &completion_) : _completion (completion_) {}
    ~completion_guard_t () { zlink_completion_close (&_completion); }
    completion_guard_t (const completion_guard_t &) = delete;
    completion_guard_t &operator= (const completion_guard_t &) = delete;

  private:
    zlink_completion_t &_completion;
};

int request_errno (request_result_t result_) noexcept
{
    switch (result_) {
        case request_result_t::timed_out: return ETIMEDOUT;
        case request_result_t::not_found: return ENOENT;
        case request_result_t::terminated: return ETERM;
        case request_result_t::protocol_error: return EPROTO;
        case request_result_t::rejected: return EACCES;
        case request_result_t::conflict: return ESTALE;
        case request_result_t::busy: return EBUSY;
        case request_result_t::not_connected: return ENOTCONN;
        case request_result_t::invalid_argument: return EINVAL;
        case request_result_t::invalid_state: return EFSM;
        case request_result_t::not_supported: return ENOTSUP;
        default: return EIO;
    }
}

submit_result_t send_terminal_result (int err_) noexcept
{
    if (err_ == ENOENT)
        return submit_result_t::not_found;
    if (err_ == ETERM || err_ == ESHUTDOWN)
        return submit_result_t::terminated;
    return submit_result_t::internal_error;
}

bool is_lifecycle_errno (int err_) noexcept
{
    return err_ == ETERM || err_ == ESHUTDOWN;
}

} // namespace

completion_entry_t::completion_entry_t (
  async_operation_state_t<void> *send_result_,
  std::unique_ptr<operation_state_t> send_operation_) :
    _kind (kind_t::send_retry), _send_result (send_result_),
    _send_operation (std::move (send_operation_))
{
}

completion_entry_t::completion_entry_t (
  async_operation_state_t<std::vector<message_t>> *request_result_) :
    _kind (kind_t::request), _request_result (request_result_)
{
}

completion_entry_t::completion_entry_t (
  const std::shared_ptr<async_operation_state_t<std::vector<message_t>>> &
    request_result_) :
    completion_entry_t (request_result_.get ())
{
}

completion_entry_t::completion_entry_t (
  async_operation_state_t<std::vector<message_t>> *request_result_,
  std::unique_ptr<operation_state_t> request_operation_) :
    _kind (kind_t::request), _request_result (request_result_),
    _request_operation (std::move (request_operation_))
{
}

completion_entry_t::~completion_entry_t ()
{
    if (_send_operation)
        restore_async_send_sources (*_send_operation);
    release_state (std::move (_send_operation));
    if (_request_operation)
        restore_async_send_sources (*_request_operation);
    release_state (std::move (_request_operation));
}

void completion_entry_t::fail_send (std::exception_ptr failure_) noexcept
{
    async_operation_state_t<void> *result = nullptr;
    std::exception_ptr failure = std::move (failure_);
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_settled)
            return;
        _failure = failure;
        _settled = true;
        result = _send_result;
        _changed.notify_all ();
    }
    if (result)
        result->fail (std::move (failure));
}

void completion_entry_t::fail_request (std::exception_ptr failure_) noexcept
{
    async_operation_state_t<std::vector<message_t>> *result = nullptr;
    std::exception_ptr failure = std::move (failure_);
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_settled)
            return;
        _failure = failure;
        _captured = true;
        _settled = true;
        result = _request_result;
        _changed.notify_all ();
    }
    if (result)
        result->fail (std::move (failure));
}

bool completion_entry_t::submit_send_attempt (bool initial_,
                                              bool defer_source_detach_)
{
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_settled)
            return true;
        _published = false;
        _completion_id = 0;
    }

    zlink_completion_id_t completion_id = 0;
    bool admitted = false;
    int submit_errno = 0;
    std::exception_ptr failure;
    try {
        admitted = submit_raw_send_state (*_send_operation, this,
                                          &completion_id, false);
        submit_errno = zlink_errno ();
    }
    catch (...) {
        failure = std::current_exception ();
    }

    if (!failure && admitted && completion_id != 0) {
        failure = std::make_exception_ptr (
          submit_error_t (submit_result_t::internal_error, EPROTO));
    } else if (!failure && !admitted
               && (submit_errno != EAGAIN || completion_id == 0)) {
        failure = std::make_exception_ptr (
          submit_error_t (submit_result_t::internal_error, EPROTO));
    }

    if (failure) {
        if (initial_)
            restore_async_send_sources (*_send_operation);
        fail_send (failure);
        if (initial_)
            std::rethrow_exception (failure);
        return true;
    }

    // async() owns every source object after it has either admitted the packet
    // or returned a live retry result. No retry entry retains caller pointers.
    if (admitted || !defer_source_detach_)
        detach_async_send_sources (*_send_operation);
    if (!admitted) {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_settled)
            return true;
        _completion_id = completion_id;
        _published = true;
        _changed.notify_all ();
        return false;
    }

    async_operation_state_t<void> *result = nullptr;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_settled)
            return true;
        _settled = true;
        result = _send_result;
        _changed.notify_all ();
    }
    if (result)
        result->complete ();
    return true;
}

bool completion_entry_t::start_send (bool defer_source_detach_)
{
    if (_kind != kind_t::send_retry || !_send_operation)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    own_async_send_parts (*_send_operation);
    return submit_send_attempt (true, defer_source_detach_);
}

void completion_entry_t::detach_send_sources () noexcept
{
    if (_send_operation)
        detach_async_send_sources (*_send_operation);
}

bool completion_entry_t::submit_request_attempt (bool initial_)
{
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_settled)
            return true;
        _published = false;
        _completion_id = 0;
    }

    zlink_completion_id_t completion_id = 0;
    bool admitted = false;
    std::exception_ptr failure;
    try {
        admitted = submit_raw_request_state (*_request_operation, this,
                                             &completion_id, false);
    }
    catch (...) {
        failure = std::current_exception ();
    }

    if (failure) {
        if (initial_)
            restore_async_send_sources (*_request_operation);
        fail_request (failure);
        if (initial_)
            std::rethrow_exception (failure);
        return true;
    }

    if (!admitted) {
        if (initial_)
            own_async_send_parts (*_request_operation);
        detach_async_send_sources (*_request_operation);
        std::lock_guard<std::mutex> lock (_mutex);
        if (_settled)
            return true;
        _completion_id = completion_id;
        _request_waiting_writable = true;
        _published = true;
        _changed.notify_all ();
        return false;
    }

    std::unique_ptr<operation_state_t> admitted_operation;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        admitted_operation = std::move (_request_operation);
        if (_settled) {
            _changed.notify_all ();
        } else {
            _completion_id = completion_id;
            _request_waiting_writable = false;
            _published = true;
            _changed.notify_all ();
        }
    }
    release_state (std::move (admitted_operation));
    return false;
}

void completion_entry_t::start_request ()
{
    if (_kind != kind_t::request || !_request_operation)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    (void) submit_request_attempt (true);
}

void completion_entry_t::publish (uint64_t completion_id_) noexcept
{
    std::unique_lock<std::mutex> lock (_mutex);
    _completion_id = completion_id_;
    _published = true;
    settle_if_joined (lock);
}

void completion_entry_t::fail_submit () noexcept
{
    std::lock_guard<std::mutex> lock (_mutex);
    _published = true;
    _captured = true;
    _settled = true;
    _changed.notify_all ();
}

bool completion_entry_t::capture (zlink_completion_t &completion_) noexcept
{
    completion_guard_t guard (completion_);

    if (_kind == kind_t::send_retry) {
        const zlink_completion_kind_t completion_kind = completion_.kind;
        const zlink_completion_id_t completion_id = completion_.completion_id;
        void *const user_context = completion_.user_context;
        const zlink_send_complete_result_t send_result = completion_.send_result;
        const int terminal_errno = completion_.send_terminal_errno;

        bool routing_id_matches = true;
        const zlink_routing_id_t *const expected_rid =
          target_first_rid_native (_send_operation->raw.target);
        if (expected_rid) {
            routing_id_matches = completion_.peer_rid.size == expected_rid->size
              && std::memcmp (completion_.peer_rid.data, expected_rid->data,
                              expected_rid->size)
                   == 0;
        } else
            routing_id_matches = completion_.peer_rid.size == 0;

        {
            std::unique_lock<std::mutex> lock (_mutex);
            _changed.wait (lock, [this] { return _published || _settled; });
            if (_settled)
                return true;
            if (completion_id != _completion_id || user_context != this)
                return false;
            _published = false;
            _completion_id = 0;
        }

        if (completion_kind != ZLINK_COMPLETION_WRITABLE
            || !routing_id_matches) {
            fail_send (std::make_exception_ptr (
              submit_error_t (submit_result_t::internal_error, EPROTO)));
            return true;
        }

        if (send_result != ZLINK_SEND_ADMITTED || terminal_errno != 0) {
            const int error = terminal_errno != 0 ? terminal_errno : EIO;
            const submit_result_t result = send_result == ZLINK_SEND_TERMINAL
              ? send_terminal_result (error)
              : submit_result_t::internal_error;
            fail_send (std::make_exception_ptr (submit_error_t (result, error)));
            return true;
        }

        try {
            return submit_send_attempt (false);
        }
        catch (...) {
            fail_send (std::current_exception ());
            return true;
        }
    }

    bool waiting_writable = false;
    {
        std::unique_lock<std::mutex> lock (_mutex);
        _changed.wait (lock, [this] { return _published || _settled; });
        if (_settled)
            return true;
        if (completion_.completion_id != _completion_id
            || completion_.user_context != this)
            return false;
        waiting_writable = _request_waiting_writable;
        if (waiting_writable) {
            _published = false;
            _completion_id = 0;
        }
    }

    if (waiting_writable) {
        bool routing_id_matches = true;
        const zlink_routing_id_t *const expected_rid =
          target_first_rid_native (_request_operation->raw.target);
        if (expected_rid) {
            routing_id_matches = completion_.peer_rid.size == expected_rid->size
              && std::memcmp (completion_.peer_rid.data, expected_rid->data,
                              expected_rid->size)
                   == 0;
        } else {
            routing_id_matches = completion_.peer_rid.size == 0;
        }

        if (completion_.kind != ZLINK_COMPLETION_WRITABLE
            || !routing_id_matches) {
            fail_request (std::make_exception_ptr (
              submit_error_t (submit_result_t::internal_error, EPROTO)));
            return true;
        }
        if (completion_.send_result != ZLINK_SEND_ADMITTED
            || completion_.send_terminal_errno != 0) {
            const int error = completion_.send_terminal_errno != 0
              ? completion_.send_terminal_errno
              : EIO;
            const submit_result_t result =
              completion_.send_result == ZLINK_SEND_TERMINAL
                ? send_terminal_result (error)
                : submit_result_t::internal_error;
            fail_request (std::make_exception_ptr (
              submit_error_t (result, error)));
            return true;
        }

        try {
            return submit_request_attempt (false);
        }
        catch (...) {
            fail_request (std::current_exception ());
            return true;
        }
    }

    std::vector<message_t> parts;
    std::exception_ptr failure;
    try {
        if (completion_.kind != ZLINK_COMPLETION_REQUEST) {
            failure = std::make_exception_ptr (
              request_error_t (request_result_t::internal_error, EPROTO));
        } else if (completion_.request_result != ZLINK_REQUEST_OK) {
            const request_result_t result =
              static_cast<request_result_t> (completion_.request_result);
            failure = std::make_exception_ptr (request_error_t (result, request_errno (result)));
        } else {
            parts.reserve (completion_.reply_part_count);
            for (size_t i = 0; i < completion_.reply_part_count; ++i) {
                message_t part;
                adopt_native_message (part, &completion_.reply_parts[i]);
                parts.push_back (std::move (part));
            }
        }
    }
    catch (...) {
        failure = std::current_exception ();
    }

    std::unique_lock<std::mutex> lock (_mutex);
    if (_captured)
        return _settled;
    _failure = std::move (failure);
    _reply_parts = std::move (parts);
    _captured = true;
    settle_if_joined (lock);
    return _settled;
}

void completion_entry_t::terminate (int terminal_errno_) noexcept
{
    if (_kind == kind_t::send_retry) {
        const int error = terminal_errno_ != 0 ? terminal_errno_ : EIO;
        fail_send (std::make_exception_ptr (
          submit_error_t (send_terminal_result (error), error)));
        return;
    }

    bool before_request_admission = false;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        before_request_admission = _request_operation != nullptr;
    }
    if (before_request_admission) {
        const int error = terminal_errno_ != 0 ? terminal_errno_ : EIO;
        fail_request (std::make_exception_ptr (
          submit_error_t (send_terminal_result (error), error)));
        return;
    }

    const int error = terminal_errno_ != 0 ? terminal_errno_ : EIO;
    const request_result_t result = is_lifecycle_errno (error)
      ? request_result_t::terminated
      : request_result_t::internal_error;
    fail_request (std::make_exception_ptr (request_error_t (result, error)));
}

void completion_entry_t::settle_if_joined (std::unique_lock<std::mutex> &lock_) noexcept
{
    if (!_published || !_captured || _settled)
        return;
    _settled = true;
    const std::exception_ptr failure = _failure;
    async_operation_state_t<std::vector<message_t>> *const request_result =
      _request_result;
    std::vector<message_t> parts;
    if (request_result)
        parts = std::move (_reply_parts);
    lock_.unlock ();
    if (request_result) {
        if (failure)
            request_result->fail (failure);
        else
            request_result->complete (std::move (parts));
    }
    lock_.lock ();
    _changed.notify_all ();
}

void completion_entry_t::wait_settled () noexcept
{
    std::unique_lock<std::mutex> lock (_mutex);
    _changed.wait (lock, [this] { return _settled; });
}

std::vector<message_t> completion_entry_t::wait_request ()
{
    std::unique_lock<std::mutex> lock (_mutex);
    _changed.wait (lock, [this] { return _settled; });
    const std::exception_ptr failure = _failure;
    std::vector<message_t> result = std::move (_reply_parts);
    lock.unlock ();
    if (failure)
        std::rethrow_exception (failure);
    return result;
}

completion_owner_t::completion_owner_t (void *socket_) :
    _socket (socket_), _entries (&_entry_map_pool),
    _early_send_completions (&_entry_map_pool)
{
}

completion_owner_t::~completion_owner_t () { shutdown (); }

void completion_owner_t::register_entry (const std::shared_ptr<completion_entry_t> &entry_)
{
    std::lock_guard<std::mutex> lock (_mutex);
    if (_shutdown)
        throw submit_error_t (submit_result_t::invalid_state, ESHUTDOWN);
    const auto inserted = _entries.emplace (entry_.get (), entry_);
    if (!inserted.second)
        throw submit_error_t (submit_result_t::invalid_state, EBUSY);
    try {
        if (!_public_owner)
            start_runtime_owner_locked ();
    }
    catch (...) {
        _entries.erase (inserted.first);
        throw;
    }
}

void completion_owner_t::register_send_entry (
  const std::shared_ptr<completion_entry_t> &entry_)
{
    if (!entry_ || entry_->kind () != completion_entry_t::kind_t::send_retry)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    zlink_completion_t early_completion{};
    bool has_early_completion = false;
    std::unique_lock<std::mutex> lock (_mutex);
    if (_shutdown)
        throw submit_error_t (submit_result_t::invalid_state, ESHUTDOWN);
    const auto inserted = _entries.emplace (entry_.get (), entry_);
    if (!inserted.second)
        throw submit_error_t (submit_result_t::invalid_state, EBUSY);
    ++_send_entry_count;

    const auto early = _early_send_completions.find (entry_.get ());
    if (early != _early_send_completions.end ()) {
        early_completion = early->second;
        _early_send_completions.erase (early);
        has_early_completion = true;
    }

    // SEND retry progress belongs to the application's public poller. Stop the
    // REQUEST fallback owner before the first DONTWAIT attempt can publish a
    // WRITABLE completion; otherwise that private thread could consume it.
    if (!_public_owner)
        stop_runtime_owner_locked (lock);
    lock.unlock ();

    if (has_early_completion && entry_->capture (early_completion))
        unregister_entry (entry_.get ());
}

void completion_owner_t::unregister_entry (completion_entry_t *entry_) noexcept
{
    try {
        std::unique_lock<std::mutex> lock (_mutex);
        const auto found = _entries.find (entry_);
        if (found == _entries.end ())
            return;
        if (found->second->kind () == completion_entry_t::kind_t::send_retry
            && _send_entry_count != 0)
            --_send_entry_count;
        _entries.erase (found);
        if (!_shutdown && !_public_owner && _send_entry_count == 0
            && !_entries.empty ())
            start_runtime_owner_locked ();
    }
    catch (...) {
    }
}

size_t completion_owner_t::drain (bool wait_for_publish_,
                                  uint64_t runtime_generation_)
{
    size_t processed = 0;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_shutdown
                || (!wait_for_publish_
                    && (_runtime_stop || _public_owner
                        || _send_entry_count != 0
                        || runtime_generation_ != _runtime_generation)))
                break;
        }
        zlink_completion_t completion{};
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          _socket, &completion, static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (rc == ZLINK_RECV_NO_DATA)
            break;
        if (rc != ZLINK_RECV_OK)
            throw recv_error_t (static_cast<recv_result_t> (rc), zlink_errno ());

        std::shared_ptr<completion_entry_t> entry;
        bool retained_early_send = false;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            const auto found = _entries.find (completion.user_context);
            if (found != _entries.end ())
                entry = found->second;
            else if (!_shutdown && completion.user_context
                     && completion.kind == ZLINK_COMPLETION_WRITABLE) {
                // async SEND submits before taking the owner/map locks. If a
                // concurrent public/runtime drain wins that short window,
                // retain its exact token until register_send_entry replays it.
                // The map node is allocated only on actual backpressure.
                const auto inserted = _early_send_completions.emplace (
                  completion.user_context, completion);
                retained_early_send = inserted.second;
            }
        }
        if (entry) {
            const bool terminal = entry->capture (completion);
            if (terminal) {
                unregister_entry (entry.get ());
            }
        } else if (!retained_early_send) {
            zlink_completion_close (&completion);
        }
        ++processed;
    }
    return processed;
}

void completion_owner_t::start_runtime_owner_locked ()
{
    if (_runtime_poller || _runtime_thread.joinable () || _shutdown
        || _send_entry_count != 0)
        return;
    _runtime_poller = zlink_poller_new ();
    if (!_runtime_poller)
        throw std::system_error (zlink_errno (), std::generic_category ());
    const zlink_config_result_t rc = zlink_poller_add (
      _runtime_poller, _socket, this, static_cast<short> (ZLINK_POLLCOMPLETION));
    if (rc != ZLINK_CONFIG_OK) {
        void *poller = _runtime_poller;
        _runtime_poller = nullptr;
        (void) zlink_poller_destroy (&poller);
        throw config_error_t (static_cast<config_result_t> (rc), zlink_errno ());
    }
    _runtime_stop = false;
    const uint64_t runtime_generation = ++_runtime_generation;
    const std::shared_ptr<completion_owner_t> self = shared_from_this ();
    try {
        _runtime_thread = std::thread (
          [self, runtime_generation] { self->runtime_loop (runtime_generation); });
    }
    catch (...) {
        void *poller = _runtime_poller;
        _runtime_poller = nullptr;
        _runtime_stop = true;
        ++_runtime_generation;
        (void) zlink_poller_destroy (&poller);
        throw;
    }
}

void completion_owner_t::stop_runtime_owner_locked (
  std::unique_lock<std::mutex> &lock_) noexcept
{
    _runtime_stop = true;
    ++_runtime_generation;
    std::thread thread = std::move (_runtime_thread);
    void *poller = _runtime_poller;
    _runtime_poller = nullptr;
    lock_.unlock ();
    if (thread.joinable ()) {
        if (thread.get_id () == std::this_thread::get_id ())
            thread.detach ();
        else
            thread.join ();
    }
    if (poller)
        (void) zlink_poller_destroy (&poller);
    lock_.lock ();
}

void completion_owner_t::runtime_loop (uint64_t runtime_generation_) noexcept
{
    while (true) {
        void *poller = nullptr;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_runtime_stop || _shutdown
                || runtime_generation_ != _runtime_generation)
                return;
            poller = _runtime_poller;
        }
        zlink_poller_event_t event{};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const int rc = zlink_poller_wait (poller, &event, 1, 25, &error);
        if (rc > 0) {
            try {
                (void) drain (false, runtime_generation_);
            }
            catch (const binding_error_t &error_) {
                shutdown (error_.internal_errno () != 0 ? error_.internal_errno () : EIO);
                return;
            }
            catch (...) {
                shutdown (EIO);
                return;
            }
        } else if (rc < 0 && zlink_errno () != EINTR && zlink_errno () != EAGAIN) {
            const int error_code = zlink_errno ();
            shutdown (error_code != 0 ? error_code : EIO);
            return;
        }
    }
}

void completion_owner_t::transfer_to_public (const void *poller_owner_)
{
    std::unique_lock<std::mutex> lock (_mutex);
    if (_shutdown)
        throw config_error_t (config_result_t::invalid_state, ESHUTDOWN);
    if (_public_owner && _public_owner != poller_owner_)
        throw config_error_t (config_result_t::invalid_state, EBUSY);
    if (!_public_owner) {
        // Publish public ownership before join drops the mutex so a concurrent
        // REQUEST registration cannot start a replacement fallback owner.
        _public_owner = poller_owner_;
        stop_runtime_owner_locked (lock);
    }
}

void completion_owner_t::transfer_to_runtime (const void *poller_owner_) noexcept
{
    try {
        std::unique_lock<std::mutex> lock (_mutex);
        if (_public_owner != poller_owner_)
            return;
        _public_owner = nullptr;
        if (!_entries.empty () && _send_entry_count == 0) {
            try {
                start_runtime_owner_locked ();
            }
            catch (...) {
                lock.unlock ();
                shutdown (EIO);
            }
        }
    }
    catch (...) {
        shutdown (EIO);
    }
}

void completion_owner_t::shutdown (int terminal_errno_) noexcept
{
    std::unique_lock<std::mutex> lock (_mutex);
    if (_shutdown)
        return;
    _shutdown = true;
    stop_runtime_owner_locked (lock);
    auto entries = std::move (_entries);
    _entries.clear ();
    for (auto &[key, completion] : _early_send_completions) {
        (void) key;
        zlink_completion_close (&completion);
    }
    _early_send_completions.clear ();
    _send_entry_count = 0;
    lock.unlock ();
    for (auto &[key, entry] : entries) {
        (void) key;
        entry->terminate (terminal_errno_);
    }
}

} // namespace zlink::detail
