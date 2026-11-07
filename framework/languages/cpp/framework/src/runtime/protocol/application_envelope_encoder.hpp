/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>

#include <functional>
#include <optional>
#include <string>

namespace zlink::framework::runtime::protocol
{
using application_envelope_encoder_t = std::function<zlink::message_t (
  const std::optional<std::string> &, std::optional<flow_origin_t>)>;
} // namespace zlink::framework::runtime::protocol
