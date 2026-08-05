/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Eventing/timers.hpp>

#include <Runtime/Eventing/timer_access.hpp>

#include <zlink.h>

namespace zlink
{

struct timer_t::impl
{
    void *handle = nullptr;
    std::function<void (uint64_t)> handler;
};

namespace detail
{

void *timer_access_t::native_handle (timer_t &timer_) noexcept
{
    return timer_._impl ? timer_._impl->handle : nullptr;
}

const void *timer_access_t::native_handle (const timer_t &timer_) noexcept
{
    return timer_._impl ? timer_._impl->handle : nullptr;
}

} // namespace detail

timer_t::timer_t () : _impl (std::make_unique<impl> ())
{
    _impl->handle = zlink_timer_new ();
}

timer_t::~timer_t ()
{
    try {
        close ();
    }
    catch (...) {
    }
}

timer_t::timer_t (timer_t &&other) noexcept = default;

timer_t &timer_t::operator= (timer_t &&other) noexcept
{
    if (this == &other)
        return *this;
    try {
        close ();
    }
    catch (...) {
    }
    _impl = std::move (other._impl);
    return *this;
}

bool timer_t::valid () const noexcept
{
    return _impl && _impl->handle != nullptr;
}

void timer_t::start_ns (uint64_t interval_ns_, uint64_t repeat_count_)
{
    detail::throw_if_failed<config_error_t> (static_cast<config_result_t> (
      zlink_timer_start (_impl->handle, interval_ns_, repeat_count_)));
}

void timer_t::stop ()
{
    detail::throw_if_failed<config_error_t> (
      static_cast<config_result_t> (zlink_timer_stop (_impl->handle)));
}

std::optional<uint64_t> timer_t::recv ()
{
    uint64_t fire_count = 0;
    const recv_result_t result =
      static_cast<recv_result_t> (zlink_timer_recv (_impl->handle, &fire_count));
    if (result == recv_result_t::no_data)
        return std::nullopt;
    if (result != recv_result_t::ok)
        throw recv_error_t (result, zlink_errno ());
    return std::optional<uint64_t> (fire_count);
}

void timer_t::on_fire (std::function<void (uint64_t)> handler_)
{
    _impl->handler = std::move (handler_);
    detail::throw_if_failed<handler_error_t> (static_cast<handler_result_t> (zlink_timer_handler (
      _impl->handle,
      [] (void *, uint64_t fire_count_, void *userdata_) {
          timer_t *self = static_cast<timer_t *> (userdata_);
          if (!self || !self->_impl || !self->_impl->handler)
              return;
          self->_impl->handler (fire_count_);
      },
      this)));
}

void timer_t::close ()
{
    if (!_impl || !_impl->handle)
        return;

    void *timer = _impl->handle;
    _impl->handle = nullptr;
    detail::throw_if_failed<close_error_t> (
      static_cast<close_result_t> (zlink_timer_destroy (&timer)));
}

} // namespace zlink
