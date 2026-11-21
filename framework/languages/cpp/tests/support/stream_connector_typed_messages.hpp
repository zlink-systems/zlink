/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "stream_connector_wire_frames.hpp"
#include <zlink/stream_connector.hpp>
#include <zlink/json.hpp>
#include <zlink/json_profile.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace stream_connector_fixture
{
struct failed_protobuf_t
{
    static constexpr const char *packet_name = "failed.protobuf";
    bool SerializeToString (std::string *bytes) const
    {
        *bytes = "partial";
        return false;
    }
    bool ParseFromString (const std::string &) { return false; }
};

inline void from_stream_payload (zlink::stream_connector::codec_t,
                                 const std::vector<std::uint8_t> &payload,
                                 failed_protobuf_t &value)
{
    if (!value.ParseFromString (std::string (payload.begin (), payload.end ())))
        throw std::runtime_error ("expected protobuf parse failure");
}

struct login_request_t
{
    static constexpr const char *packet_name = "LoginRequest";
};

struct login_reply_t
{
};

struct auto_payload_t
{
    static constexpr const char *packet_name = "AutoPayload";
    std::string text;
};

template <typename T>
concept has_core_async_terminator = requires (T value) { value.async (); };
} // namespace stream_connector_fixture

template <> struct zlink::json::dto_traits<stream_connector_fixture::auto_payload_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("text", &stream_connector_fixture::auto_payload_t::text)};
};

template <> struct zlink::json::dto_traits<stream_connector_fixture::login_request_t>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct zlink::json::dto_traits<stream_connector_fixture::login_reply_t>
{
    static constexpr auto fields = std::tuple{};
};

namespace stream_connector_fixture
{
inline std::vector<std::uint8_t> to_stream_payload (const auto_payload_t &payload)
{
    return as_bytes (zlink::detail::json_profile::encode (payload));
}

inline void from_stream_payload (const std::vector<std::uint8_t> &payload, auto_payload_t &message)
{
    message =
      zlink::detail::json_profile::decode<auto_payload_t> (payload.begin (), payload.end ());
}
} // namespace stream_connector_fixture
