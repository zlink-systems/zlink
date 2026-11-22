/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace zlink::framework::runtime::messaging
{
class envelope_codec_t;
}

namespace zlink::framework::runtime::protocol
{
class application_envelope_encoder_t
{
  public:
    zlink::message_t operator() (const std::optional<std::string> &flow_id,
                                 std::optional<flow_origin_t> flow_origin) const;

  private:
    friend class messaging::envelope_codec_t;
    struct state_t;
    explicit application_envelope_encoder_t (std::shared_ptr<const state_t> state) :
        _state (std::move (state))
    {
    }

    std::shared_ptr<const state_t> _state;
};
} // namespace zlink::framework::runtime::protocol
