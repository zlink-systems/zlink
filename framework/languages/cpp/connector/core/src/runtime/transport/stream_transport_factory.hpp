/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/zlink_stream_connector_options.hpp>

namespace zlink::stream_connector::detail
{

class stream_transport_factory_t
{
  public:
    static bool is_supported (transport_t transport) noexcept;
};

} // namespace zlink::stream_connector::detail
