/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/zlink_stream_connector.hpp>

namespace zlink::stream_connector
{

class connector_factory_t
{
  public:
    static connector_t create (connector_options_t options);
};

} // namespace zlink::stream_connector
