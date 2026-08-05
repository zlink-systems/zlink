/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/connector_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zlink::stream_connector::detail
{

class frame_codec_t
{
  public:
    static bool validate_frame_size (std::size_t header_size,
                                     std::size_t payload_size,
                                     const connector_options_t &options);
    static bool validate_receive_frame_size (std::size_t header_size,
                                             std::size_t payload_size,
                                             const connector_options_t &options);
    static result_t<std::vector<std::uint8_t>> encode_prefix (std::size_t header_size,
                                                              std::size_t payload_size);
    static result_t<std::vector<std::uint8_t>> encode (const std::vector<std::uint8_t> &header,
                                                       const std::vector<std::uint8_t> &payload,
                                                       const connector_options_t &options);
};

} // namespace zlink::stream_connector::detail
