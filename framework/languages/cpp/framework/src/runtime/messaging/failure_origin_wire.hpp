/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/messaging/envelope_codec.hpp"

#include <zlink/framework/contracts/errors/error.hpp>

#include <map>
#include <string>
#include <string_view>

namespace zlink::framework::runtime::messaging
{

inline constexpr std::string_view failure_origin_metadata_key =
  "__zlink.failureOrigin";

inline std::string_view failure_origin_wire_name (
  detail::failure_origin_t origin) noexcept
{
    switch (origin) {
        case detail::failure_origin_t::payload_encode:
            return "payload_encode";
        case detail::failure_origin_t::payload_decode:
            return "payload_decode";
        case detail::failure_origin_t::actor_transfer_in_progress:
            return "actor_transfer_in_progress";
        case detail::failure_origin_t::none:
            return {};
    }
    return {};
}

inline detail::failure_origin_t failure_origin_from_wire (
  std::string_view value) noexcept
{
    if (value == "payload_encode")
        return detail::failure_origin_t::payload_encode;
    if (value == "payload_decode")
        return detail::failure_origin_t::payload_decode;
    if (value == "actor_transfer_in_progress")
        return detail::failure_origin_t::actor_transfer_in_progress;
    return detail::failure_origin_t::none;
}

inline void write_failure_origin (
  envelope_header_t &header,
  const framework_exception_t &error)
{
    const auto name = failure_origin_wire_name (
      detail::failure_origin (error));
    if (!name.empty ())
        header.metadata.insert_or_assign (
          std::string (failure_origin_metadata_key), std::string (name));
}

/* Framework-origin error marker: attached only to error replies the
 * framework itself produced (route resolution failure, sealed admission,
 * dispatch rejection). Application handler errors never carry it. Stale-route
 * judgement on the caller side requires this marker so an application error
 * kind cannot be mistaken for a stale-route control signal. */
inline constexpr std::string_view framework_origin_metadata_key = "zlink.origin";
inline constexpr std::string_view framework_origin_metadata_value = "framework";

inline void write_error_origin (envelope_header_t &header, const framework_exception_t &error)
{
    if (framework::detail::error_origin (error) == framework::detail::error_origin_t::framework)
        header.metadata.insert_or_assign (std::string (framework_origin_metadata_key),
                                          std::string (framework_origin_metadata_value));
}

inline bool has_framework_origin (const std::map<std::string, std::string> &metadata)
{
    const auto found = metadata.find (std::string (framework_origin_metadata_key));
    return found != metadata.end () && found->second == framework_origin_metadata_value;
}

inline framework_exception_t restore_failure_origin (
  const envelope_header_t &header,
  framework_exception_t error)
{
    const auto found = header.metadata.find (
      std::string (failure_origin_metadata_key));
    if (found == header.metadata.end ())
        return error;
    const auto origin = failure_origin_from_wire (found->second);
    return origin == detail::failure_origin_t::none
      ? error
      : detail::make_origin_exception (
          error.kind (), origin, error.what ());
}

} // namespace zlink::framework::runtime::messaging
