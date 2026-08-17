/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/errors/error.hpp>

#include "runtime/diagnostics/dispatch_events.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace zlink::framework::runtime
{

/* flow_id wire form (flow-correlation §6): lowercase hyphenated UUIDv7,
 * 36 ASCII bytes. The generation algorithm is a framework-internal decision;
 * ids are created once per flow at the entry point and never per hop. */
class flow_id_t
{
  public:
    static constexpr std::uint8_t format_marker = 0xF2;
    static constexpr std::size_t encoded_length = 36;

    static std::string create ();
    static bool is_valid (std::string_view value) noexcept;
};

struct flow_value_t
{
    std::string flow_id;
    flow_origin_t origin = flow_origin_t::inbound;
    message_flow_log_mode_t diagnostics_mode =
      message_flow_log_mode_t::off;
};

/* Ambient flow of the framework-invoked callback currently running on this
 * thread (flow-correlation §4.4). Scopes restore the previous value so a
 * finished callback never leaks its id into the next unrelated callback. */
class flow_context_t
{
  public:
    class scope_t
    {
      public:
        explicit scope_t (std::optional<flow_value_t> value) : _previous (slot ())
        {
            slot () = std::move (value);
        }

        scope_t (scope_t &&other) noexcept :
            _previous (std::move (other._previous)), _armed (other._armed)
        {
            other._armed = false;
        }

        scope_t (const scope_t &) = delete;
        scope_t &operator= (const scope_t &) = delete;
        scope_t &operator= (scope_t &&) = delete;

        ~scope_t ()
        {
            if (_armed) {
                slot () = std::move (_previous);
            }
        }

      private:
        std::optional<flow_value_t> _previous;
        bool _armed = true;
    };

    static const std::optional<flow_value_t> &current () noexcept { return slot (); }

    /* Enters the supplied wire pair, or creates a new flow when absent and
     * capture is enabled. Both fields must be present together. */
    static scope_t enter (const std::optional<std::string> &flow_id,
                          std::optional<flow_origin_t> origin,
                          message_flow_log_mode_t diagnostics_mode,
                          flow_origin_t default_origin)
    {
        // Flow fields are observation-only.  At Off the processing point must
        // neither validate nor install inbound flow state (spec 27 §4).
        if (diagnostics_mode == message_flow_log_mode_t::off) {
            return scope_t (std::nullopt);
        }
        if (flow_id.has_value () != origin.has_value ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "flow id and origin must be present together");
        }
        if (flow_id) {
            if (!flow_id_t::is_valid (*flow_id)) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "flow id must be UUIDv7");
            }
            return scope_t (flow_value_t{
              *flow_id, *origin, diagnostics_mode});
        }
        return scope_t (flow_value_t{
          flow_id_t::create (), default_origin, diagnostics_mode});
    }

    static scope_t enter (std::optional<std::string> &&flow_id,
                          std::optional<flow_origin_t> origin,
                          message_flow_log_mode_t diagnostics_mode,
                          flow_origin_t default_origin)
    {
        if (diagnostics_mode == message_flow_log_mode_t::off)
            return scope_t (std::nullopt);
        if (flow_id.has_value () != origin.has_value ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "flow id and origin must be present together");
        }
        if (flow_id) {
            if (!flow_id_t::is_valid (*flow_id)) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "flow id must be UUIDv7");
            }
            return scope_t (flow_value_t{
              std::move (*flow_id), *origin, diagnostics_mode});
        }
        return scope_t (flow_value_t{
          flow_id_t::create (), default_origin, diagnostics_mode});
    }

    static scope_t enter_current_or_create (
      flow_origin_t origin, message_flow_log_mode_t diagnostics_mode)
    {
        if (diagnostics_mode == message_flow_log_mode_t::off)
            return scope_t (std::nullopt);
        const auto &value = current ();
        if (value) {
            return scope_t (*value);
        }
        return scope_t (flow_value_t{
          flow_id_t::create (), origin, diagnostics_mode});
    }

  private:
    static std::optional<flow_value_t> &slot () noexcept
    {
        thread_local std::optional<flow_value_t> value;
        return value;
    }
};

} // namespace zlink::framework::runtime
