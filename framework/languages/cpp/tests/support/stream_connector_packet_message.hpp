/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/stream_connector.hpp>

namespace stream_connector_fixture
{
using packet_message_t = zlink::stream_connector::message_t<zlink::stream_connector::packet_t>;
} // namespace stream_connector_fixture
