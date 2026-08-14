/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/channel_runtime.hpp"

#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/channels/channel.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <typeindex>

namespace zlink::framework::detail
{

class channel_outbound_exchange_t
{
  public:
    explicit channel_outbound_exchange_t (std::shared_ptr<channel_runtime_state_t> state);

    task_t<zlink::message_t>
    submit_request (std::string channel_name,
                    std::string packet_name,
                    std::type_index request_type,
                    message_bus_t::payload_encoder_t encode_payload,
                    std::chrono::milliseconds timeout,
                    const channel_request_call_t::metadata_map_t &metadata);

    task_t<void> submit_send (std::string channel_name,
                              std::string packet_name,
                              std::type_index message_type,
                              message_bus_t::payload_encoder_t encode_payload,
                              std::chrono::milliseconds timeout,
                              const send_call_t::metadata_map_t &metadata);

    task_t<void> submit_publish (std::string channel_name,
                                   std::string topic,
                                   std::string packet_name,
                                   std::type_index event_type,
                                   message_bus_t::payload_encoder_t encode_payload,
                                   std::chrono::milliseconds timeout,
                                   const send_call_t::metadata_map_t &metadata);

  private:
    std::shared_ptr<channel_runtime_state_t> _state;
};

void close_native_channel_transports (
  const std::shared_ptr<channel_runtime_state_t> &state) noexcept;

void initialize_manual_channel_publishers (
  const std::shared_ptr<channel_runtime_state_t> &state);

void close_manual_channel_publishers (
  const std::shared_ptr<channel_runtime_state_t> &state) noexcept;

} // namespace zlink::framework::detail
