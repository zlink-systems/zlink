/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Core/utilities.hpp>

#include <zlink.h>

#include <cerrno>
#include <exception>
#include <utility>

namespace zlink
{

stopwatch_t::stopwatch_t () : _handle (zlink_stopwatch_start ())
{
    if (!_handle)
        throw config_error_t (config_result_t::invalid_handle, zlink_errno ());
}

stopwatch_t::~stopwatch_t ()
{
    close ();
}

stopwatch_t::stopwatch_t (stopwatch_t &&other_) noexcept : _handle (other_._handle)
{
    other_._handle = nullptr;
}

stopwatch_t &stopwatch_t::operator= (stopwatch_t &&other_) noexcept
{
    if (this == &other_)
        return *this;
    close ();
    _handle = other_._handle;
    other_._handle = nullptr;
    return *this;
}

bool stopwatch_t::valid () const noexcept
{
    return _handle != nullptr;
}

uint64_t stopwatch_t::intermediate () const
{
    if (!_handle)
        throw config_error_t (config_result_t::invalid_handle, zlink_errno ());
    return static_cast<uint64_t> (zlink_stopwatch_intermediate (_handle));
}

uint64_t stopwatch_t::stop ()
{
    if (!_handle)
        throw config_error_t (config_result_t::invalid_handle, zlink_errno ());
    void *handle = _handle;
    _handle = nullptr;
    return static_cast<uint64_t> (zlink_stopwatch_stop (handle));
}

void stopwatch_t::close ()
{
    if (_handle)
        (void) stop ();
}

atomic_counter_t::atomic_counter_t () : _handle (zlink_atomic_counter_new ())
{
    if (!_handle)
        throw config_error_t (config_result_t::invalid_handle, zlink_errno ());
}

atomic_counter_t::~atomic_counter_t ()
{
    close ();
}

atomic_counter_t::atomic_counter_t (atomic_counter_t &&other_) noexcept : _handle (other_._handle)
{
    other_._handle = nullptr;
}

atomic_counter_t &atomic_counter_t::operator= (atomic_counter_t &&other_) noexcept
{
    if (this == &other_)
        return *this;
    close ();
    _handle = other_._handle;
    other_._handle = nullptr;
    return *this;
}

bool atomic_counter_t::valid () const noexcept
{
    return _handle != nullptr;
}

void atomic_counter_t::set (int value_)
{
    if (!_handle)
        throw config_error_t (config_result_t::invalid_handle, zlink_errno ());
    zlink_atomic_counter_set (_handle, value_);
}

int atomic_counter_t::increment ()
{
    if (!_handle)
        throw config_error_t (config_result_t::invalid_handle, zlink_errno ());
    return zlink_atomic_counter_inc (_handle);
}

int atomic_counter_t::decrement ()
{
    if (!_handle)
        throw config_error_t (config_result_t::invalid_handle, zlink_errno ());
    return zlink_atomic_counter_dec (_handle);
}

int atomic_counter_t::value () const
{
    if (!_handle)
        throw config_error_t (config_result_t::invalid_handle, zlink_errno ());
    return zlink_atomic_counter_value (_handle);
}

void atomic_counter_t::close ()
{
    if (!_handle)
        return;
    void *handle = _handle;
    _handle = nullptr;
    zlink_atomic_counter_destroy (&handle);
}

struct thread_t::state
{
    explicit state (std::function<void ()> task_) : task (std::move (task_)) {}

    std::function<void ()> task;
    std::exception_ptr exception;
};

namespace
{

void thread_entry (void *userdata_)
{
    thread_t::state *state = static_cast<thread_t::state *> (userdata_);
    try {
        state->task ();
    }
    catch (...) {
        state->exception = std::current_exception ();
    }
}

} // namespace

thread_t::thread_t (std::function<void ()> task_) :
    _state (std::make_unique<state> (std::move (task_)))
{
    if (!_state->task)
        throw config_error_t (config_result_t::invalid_argument, EINVAL);
    _handle = zlink_thread_start (&thread_entry, _state.get ());
    if (!_handle)
        throw config_error_t (config_result_t::invalid_handle, zlink_errno ());
}

thread_t::~thread_t ()
{
    close ();
}

thread_t::thread_t (thread_t &&other_) noexcept :
    _handle (other_._handle), _state (std::move (other_._state))
{
    other_._handle = nullptr;
}

thread_t &thread_t::operator= (thread_t &&other_) noexcept
{
    if (this == &other_)
        return *this;
    close ();
    _handle = other_._handle;
    _state = std::move (other_._state);
    other_._handle = nullptr;
    return *this;
}

bool thread_t::valid () const noexcept
{
    return _handle != nullptr;
}

void thread_t::join ()
{
    if (!_handle)
        return;
    void *handle = _handle;
    _handle = nullptr;
    zlink_thread_join (handle);
    if (_state && _state->exception)
        std::rethrow_exception (_state->exception);
}

void thread_t::close ()
{
    if (!_handle)
        return;
    try {
        join ();
    }
    catch (...) {
    }
}

} // namespace zlink
