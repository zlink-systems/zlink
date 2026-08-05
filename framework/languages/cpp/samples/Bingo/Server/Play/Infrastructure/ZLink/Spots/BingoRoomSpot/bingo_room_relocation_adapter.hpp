/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "bingo_room_spot.hpp"

namespace zlink::samples::bingo
{

class bingo_room_relocation_adapter_t final
    : public spot_relocation_adapter_t<bingo_room_spot_t>
{
  public:
    task_t<std::vector<std::byte>>
    capture (bingo_room_spot_t &spot,
             std::stop_token) override
    {
        const auto message =
          zlink::message_t::from_json (spot.relocation_state ());
        co_return std::vector<std::byte> (
          message.bytes ().begin (), message.bytes ().end ());
    }

    task_t<void>
    restore (bingo_room_spot_t &spot,
             std::vector<std::byte> payload,
             std::stop_token) override
    {
        const auto message = zlink::message_t::from (
          std::span<const std::byte> (
            payload.data (), payload.size ()));
        spot.restore_relocation_state (
          message.parse_json<bingo_room_relocation_state_t> ());
        co_return;
    }
};

} // namespace zlink::samples::bingo
