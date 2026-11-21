/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "stream_connector_wire_frames.hpp"
#include <zlink/stream_connector.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace stream_connector_fixture
{
class prefix_compression_codec_t final : public zlink::stream_connector::compression_codec_t
{
  public:
    explicit prefix_compression_codec_t (std::string prefix) : _prefix (std::move (prefix)) {}

    std::vector<std::uint8_t> compress (const std::vector<std::uint8_t> &payload) const override
    {
        return as_bytes (_prefix + ":" + as_string (payload));
    }

    std::vector<std::uint8_t> decompress (const std::vector<std::uint8_t> &payload,
                                          std::size_t) const override
    {
        const auto value = as_string (payload);
        const auto marker = _prefix + ":";
        if (value.rfind (marker, 0) != 0) {
            throw std::runtime_error ("unexpected compression marker");
        }
        return as_bytes (value.substr (marker.size ()));
    }

  private:
    std::string _prefix;
};
} // namespace stream_connector_fixture
