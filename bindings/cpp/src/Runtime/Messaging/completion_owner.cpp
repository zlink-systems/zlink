/* SPDX-License-Identifier: MPL-2.0 */
#include "completion_owner.hpp"

#include <Runtime/Native/message_access.hpp>

#include <zlink.h>

#include <cerrno>
#include <chrono>
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

} // namespace

completion_entry_t::completion_entry_t (
  std::shared_ptr<async_operation_state_t<void>> send_result_) :
    _kind (kind_t::send), _send_result (std::move (send_result_))
{
}

completion_entry_t::completion_entry_t (
  std::shared_ptr<async_operation_state_t<std::vector<message_t>>> request_result_) :
    _kind (kind_t::request), _request_result (std::move (request_result_))
{
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

void completion_entry_t::capture (zlink_completion_t &completion_) noexcept
{
    completion_guard_t guard (completion_);
    std::vector<message_t> parts;
    std::exception_ptr failure;
    try {
        if (_kind == kind_t::send) {
            if (completion_.kind != ZLINK_COMPLETION_SEND
                || completion_.send_result != ZLINK_SEND_ADMITTED) {
                failure = std::make_exception_ptr (submit_error_t (
                  submit_result_t::not_admitted,
                  completion_.send_terminal_errno ? completion_.send_terminal_errno : EIO));
            }
        } else if (completion_.kind != ZLINK_COMPLETION_REQUEST) {
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
        return;
    _failure = std::move (failure);
    _reply_parts = std::move (parts);
    _captured = true;
    settle_if_joined (lock);
}

void completion_entry_t::settle_if_joined (std::unique_lock<std::mutex> &lock_) noexcept
{
    if (!_published || !_captured || _settled)
        return;
    _settled = true;
    const std::exception_ptr failure = _failure;
    auto send_result = _send_result;
    auto request_result = _request_result;
    std::vector<message_t> parts;
    if (request_result)
        parts = std::move (_reply_parts);
    lock_.unlock ();
    if (_kind == kind_t::send) {
        if (failure)
            send_result->fail (failure);
        else
            send_result->complete ();
    } else if (request_result) {
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

completion_owner_t::completion_owner_t (void *socket_) : _socket (socket_) {}

completion_owner_t::~completion_owner_t () { shutdown (); }

void completion_owner_t::register_entry (const std::shared_ptr<completion_entry_t> &entry_)
{
    std::lock_guard<std::mutex> lock (_mutex);
    if (_shutdown)
        throw submit_error_t (submit_result_t::invalid_state, ESHUTDOWN);
    _entries.emplace (entry_.get (), entry_);
    if (!_public_owner)
        start_runtime_owner_locked ();
}

void completion_owner_t::unregister_entry (completion_entry_t *entry_) noexcept
{
    std::lock_guard<std::mutex> lock (_mutex);
    _entries.erase (entry_);
}

size_t completion_owner_t::drain (bool wait_for_publish_)
{
    size_t processed = 0;
    for (;;) {
        zlink_completion_t completion{};
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          _socket, &completion, static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (rc == ZLINK_RECV_NO_DATA)
            break;
        if (rc != ZLINK_RECV_OK)
            throw recv_error_t (static_cast<recv_result_t> (rc), zlink_errno ());

        std::shared_ptr<completion_entry_t> entry;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            const auto found = _entries.find (completion.user_context);
            if (found != _entries.end ())
                entry = found->second;
        }
        if (entry) {
            entry->capture (completion);
            if (wait_for_publish_)
                entry->wait_settled ();
            unregister_entry (entry.get ());
        } else {
            zlink_completion_close (&completion);
        }
        ++processed;
    }
    return processed;
}

void completion_owner_t::start_runtime_owner_locked ()
{
    if (_runtime_poller || _runtime_thread.joinable () || _shutdown)
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
    const std::shared_ptr<completion_owner_t> self = shared_from_this ();
    _runtime_thread = std::thread ([self] { self->runtime_loop (); });
}

void completion_owner_t::stop_runtime_owner_locked (
  std::unique_lock<std::mutex> &lock_) noexcept
{
    _runtime_stop = true;
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

void completion_owner_t::runtime_loop () noexcept
{
    while (true) {
        void *poller = nullptr;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_runtime_stop || _shutdown)
                return;
            poller = _runtime_poller;
        }
        zlink_poller_event_t event{};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const int rc = zlink_poller_wait (poller, &event, 1, 25, &error);
        if (rc > 0) {
            try {
                (void) drain (false);
            }
            catch (...) {
                return;
            }
        } else if (rc < 0 && zlink_errno () != EINTR && zlink_errno () != EAGAIN) {
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
        stop_runtime_owner_locked (lock);
        _public_owner = poller_owner_;
    }
}

void completion_owner_t::transfer_to_runtime (const void *poller_owner_) noexcept
{
    try {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_public_owner != poller_owner_)
            return;
        _public_owner = nullptr;
        if (!_entries.empty ())
            start_runtime_owner_locked ();
    }
    catch (...) {
    }
}

void completion_owner_t::shutdown () noexcept
{
    std::unique_lock<std::mutex> lock (_mutex);
    if (_shutdown)
        return;
    _shutdown = true;
    stop_runtime_owner_locked (lock);
    auto entries = std::move (_entries);
    _entries.clear ();
    lock.unlock ();
    for (auto &[key, entry] : entries) {
        (void) key;
        zlink_completion_t completion{};
        completion.struct_size = sizeof (completion);
        completion.kind = entry->kind () == completion_entry_t::kind_t::send
                            ? ZLINK_COMPLETION_SEND : ZLINK_COMPLETION_REQUEST;
        completion.send_result = ZLINK_SEND_TERMINAL;
        completion.send_terminal_errno = ESHUTDOWN;
        completion.request_result = ZLINK_REQUEST_TERMINATED;
        entry->publish (0);
        entry->capture (completion);
    }
}

} // namespace zlink::detail
