/* SPDX-License-Identifier: MPL-2.0 */
#include "zlink/Contracts/Core/context.hpp"

#include <Runtime/Core/context_access.hpp>
#include <Runtime/Core/duration_conversion.hpp>
#include <Runtime/Options/option_ids.hpp>

#include <zlink.h>

#include <cerrno>

namespace zlink
{

namespace
{

int context_option_id_value (detail::context_option_id option_) noexcept
{
    return static_cast<int> (option_);
}

} // namespace

struct context_t::impl
{
    void *ctx = nullptr;
    std::string thread_name_prefix;
};

context_t::context_t () : _impl (std::make_unique<impl> ())
{
    _impl->ctx = zlink_ctx_new ();
}

context_t::context_t (io_thread_count_t io_threads_) : _impl (std::make_unique<impl> ())
{
    _impl->ctx = zlink_ctx_new ();
    if (_impl->ctx)
        (void) zlink_ctx_set (
          _impl->ctx,
          static_cast<zlink_ctx_option_t> (context_option_id_value (detail::context_option_id::io_threads)),
          io_threads_.value ());
}

context_t::~context_t ()
{
    term_noexcept ();
}

context_t::context_t (context_t &&other_) noexcept : _impl (std::move (other_._impl))
{
    if (!other_._impl)
        other_._impl = std::make_unique<impl> ();
}

context_t &context_t::operator= (context_t &&other_) noexcept
{
    if (this == &other_)
        return *this;
    term_noexcept ();
    _impl = std::move (other_._impl);
    if (!other_._impl)
        other_._impl = std::make_unique<impl> ();
    return *this;
}

bool context_t::valid () const noexcept
{
    return _impl && _impl->ctx != nullptr;
}

void context_t::shutdown ()
{
    if (!_impl || !_impl->ctx)
        throw close_error_t (close_result_t::invalid_handle);
    detail::throw_if_failed<close_error_t> (
      static_cast<close_result_t> (zlink_ctx_shutdown (_impl->ctx)));
}

void context_t::term ()
{
    if (!_impl || !_impl->ctx)
        return;
    void *ctx = _impl->ctx;
    const auto result = static_cast<close_result_t> (zlink_ctx_term (ctx));
    detail::throw_if_failed<close_error_t> (result);
    _impl->ctx = nullptr;
}

void context_t::recalculate_auto_hwm ()
{
    if (!_impl || !_impl->ctx)
        throw config_error_t (config_result_t::invalid_handle);
    detail::throw_if_failed<config_error_t> (
      static_cast<config_result_t> (zlink_ctx_auto_hwm_recalculate (_impl->ctx)));
}

void context_t::term_noexcept () noexcept
{
    if (!_impl || !_impl->ctx)
        return;
    void *ctx = _impl->ctx;
    if (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK) {
        _impl->ctx = nullptr;
    }
}

int context_t::get_option_raw (int option_, int *error_out_) const
{
    zlink_config_result_t error = static_cast<zlink_config_result_t> (0);
    if (!_impl || !_impl->ctx) {
        error =
          static_cast<zlink_config_result_t> (static_cast<int> (config_result_t::invalid_handle));
        if (error_out_)
            *error_out_ = static_cast<int> (error);
        return -1;
    }
    const int value = zlink_ctx_get (_impl->ctx, static_cast<zlink_ctx_option_t> (option_), &error);
    if (error_out_)
        *error_out_ = static_cast<int> (error);
    return value;
}

config_result_t context_t::set_option_raw (int option_, int value_)
{
    if (!_impl || !_impl->ctx)
        return config_result_t::invalid_handle;
    return static_cast<config_result_t> (
      zlink_ctx_set (_impl->ctx, static_cast<zlink_ctx_option_t> (option_), value_));
}

config_result_t context_t::set_option_data_raw (int option_, std::string_view value_)
{
    if (!_impl || !_impl->ctx)
        return config_result_t::invalid_handle;
    return static_cast<config_result_t> (zlink_ctx_set_data (
      _impl->ctx, static_cast<zlink_ctx_option_t> (option_), value_.data (), value_.size ()));
}

uint64_t context_t::get_option_uint64_raw (int option_) const
{
    if (!_impl || !_impl->ctx)
        throw config_error_t (config_result_t::invalid_handle);
    uint64_t value = 0;
    size_t size = sizeof (value);
    const config_result_t result = static_cast<config_result_t> (zlink_ctx_get_data (
      _impl->ctx, static_cast<zlink_ctx_option_t> (option_), &value, &size));
    detail::throw_if_failed<config_error_t> (result);
    if (size != sizeof (value))
        throw config_error_t (config_result_t::invalid_argument, EINVAL);
    return value;
}

config_result_t context_t::set_option_uint64_raw (int option_, uint64_t value_)
{
    if (!_impl || !_impl->ctx)
        return config_result_t::invalid_handle;
    return static_cast<config_result_t> (zlink_ctx_set_data (
      _impl->ctx, static_cast<zlink_ctx_option_t> (option_), &value_, sizeof (value_)));
}

namespace detail
{

void *context_access_t::native_handle (context_t &ctx_) noexcept
{
    return ctx_._impl ? ctx_._impl->ctx : nullptr;
}

const void *context_access_t::native_handle (const context_t &ctx_) noexcept
{
    return ctx_._impl ? ctx_._impl->ctx : nullptr;
}

} // namespace detail

io_thread_count_t context_options_t::io_threads () const
{
    int error = 0;
    const int value =
      _ctx.get_option_raw (context_option_id_value (detail::context_option_id::io_threads), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    return io_thread_count_t::value (value);
}

void context_options_t::io_threads (io_thread_count_t value_)
{
    detail::throw_if_failed<config_error_t> (_ctx.set_option_raw (
      context_option_id_value (detail::context_option_id::io_threads), value_.value ()));
}

socket_count_t context_options_t::max_sockets () const
{
    int error = 0;
    const int value =
      _ctx.get_option_raw (context_option_id_value (detail::context_option_id::max_sockets), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    return socket_count_t::value (value);
}

void context_options_t::max_sockets (socket_count_t value_)
{
    detail::throw_if_failed<config_error_t> (_ctx.set_option_raw (
      context_option_id_value (detail::context_option_id::max_sockets), value_.value ()));
}

byte_size_t context_options_t::max_msg_size () const
{
    int error = 0;
    const int value =
      _ctx.get_option_raw (context_option_id_value (detail::context_option_id::max_msgsz), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    return byte_size_t::bytes (value);
}

void context_options_t::max_msg_size (byte_size_t value_)
{
    detail::throw_if_failed<config_error_t> (
      _ctx.set_option_raw (context_option_id_value (detail::context_option_id::max_msgsz),
                           static_cast<int> (value_.bytes ())));
}

std::optional<thread_priority_t> context_options_t::thread_priority () const
{
    int error = 0;
    const int value =
      _ctx.get_option_raw (context_option_id_value (detail::context_option_id::thread_priority), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    if (value == -1)
        return std::nullopt;
    return thread_priority_t::value (value);
}

void context_options_t::thread_priority (thread_priority_t value_)
{
    detail::throw_if_failed<config_error_t> (_ctx.set_option_raw (
      context_option_id_value (detail::context_option_id::thread_priority), value_.value ()));
}

thread_scheduling_policy_t context_options_t::thread_scheduling_policy () const
{
    int error = 0;
    const int value =
      _ctx.get_option_raw (context_option_id_value (detail::context_option_id::thread_sched_policy), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    return static_cast<thread_scheduling_policy_t> (value);
}

void context_options_t::thread_scheduling_policy (thread_scheduling_policy_t value_)
{
    detail::throw_if_failed<config_error_t> (_ctx.set_option_raw (
      context_option_id_value (detail::context_option_id::thread_sched_policy), static_cast<int> (value_)));
}

std::string context_options_t::thread_name_prefix () const
{
    return _ctx._impl ? _ctx._impl->thread_name_prefix : std::string ();
}

void context_options_t::thread_name_prefix (const std::string &value_)
{
    detail::validate_bounded_c_string (value_, 16u, "thread_name_prefix");
    detail::throw_if_failed<config_error_t> (_ctx.set_option_data_raw (
      context_option_id_value (detail::context_option_id::thread_name_prefix), value_));
    _ctx._impl->thread_name_prefix = value_;
}

bool context_options_t::blocky () const
{
    int error = 0;
    const int value =
      _ctx.get_option_raw (context_option_id_value (detail::context_option_id::blocky), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    return value != 0;
}

void context_options_t::blocky (bool enabled_)
{
    detail::throw_if_failed<config_error_t> (_ctx.set_option_raw (
      context_option_id_value (detail::context_option_id::blocky), enabled_ ? 1 : 0));
}

bool context_options_t::auto_hwm_enabled () const
{
    int error = 0;
    const int value =
      _ctx.get_option_raw (context_option_id_value (detail::context_option_id::auto_hwm_enable), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    return value != 0;
}

void context_options_t::auto_hwm_enabled (bool enabled_)
{
    detail::throw_if_failed<config_error_t> (_ctx.set_option_raw (
      context_option_id_value (detail::context_option_id::auto_hwm_enable), enabled_ ? 1 : 0));
}

std::chrono::milliseconds context_options_t::auto_hwm_recalc_debounce () const
{
    int error = 0;
    const int value = _ctx.get_option_raw (
      context_option_id_value (detail::context_option_id::auto_hwm_recalc_debounce_ms), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    return std::chrono::milliseconds (value);
}

void context_options_t::auto_hwm_recalc_debounce (std::chrono::milliseconds value_)
{
    detail::throw_if_failed<config_error_t> (
      _ctx.set_option_raw (context_option_id_value (detail::context_option_id::auto_hwm_recalc_debounce_ms),
                           detail::native_option_ms (value_)));
}

zlink::auto_hwm_profile context_options_t::auto_hwm_profile () const
{
    int error = 0;
    const int value =
      _ctx.get_option_raw (context_option_id_value (detail::context_option_id::auto_hwm_profile), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    return static_cast<zlink::auto_hwm_profile> (value);
}

void context_options_t::auto_hwm_profile (zlink::auto_hwm_profile profile_)
{
    detail::throw_if_failed<config_error_t> (_ctx.set_option_raw (
      context_option_id_value (detail::context_option_id::auto_hwm_profile), static_cast<int> (profile_)));
}

byte_count_t context_options_t::auto_hwm_msg_unit_bytes () const
{
    return byte_count_t::bytes (_ctx.get_option_uint64_raw (
      context_option_id_value (detail::context_option_id::auto_hwm_msg_unit_bytes)));
}

void context_options_t::auto_hwm_msg_unit_bytes (byte_count_t value_)
{
    detail::throw_if_failed<config_error_t> (_ctx.set_option_uint64_raw (
      context_option_id_value (detail::context_option_id::auto_hwm_msg_unit_bytes),
      value_.bytes ()));
}

socket_count_t context_options_t::socket_limit () const
{
    int error = 0;
    const int value =
      _ctx.get_option_raw (context_option_id_value (detail::context_option_id::socket_limit), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    return socket_count_t::value (value);
}

byte_size_t context_options_t::msg_t_size () const
{
    int error = 0;
    const int value =
      _ctx.get_option_raw (context_option_id_value (detail::context_option_id::msg_t_size), &error);
    if (error != 0)
        throw config_error_t (static_cast<config_result_t> (error));
    return byte_size_t::bytes (value);
}

void context_options_t::add_thread_affinity (cpu_index_t cpu_)
{
    detail::throw_if_failed<config_error_t> (_ctx.set_option_raw (
      context_option_id_value (detail::context_option_id::thread_affinity_cpu_add), cpu_.value ()));
}

void context_options_t::remove_thread_affinity (cpu_index_t cpu_)
{
    detail::throw_if_failed<config_error_t> (_ctx.set_option_raw (
      context_option_id_value (detail::context_option_id::thread_affinity_cpu_remove), cpu_.value ()));
}

} // namespace zlink
