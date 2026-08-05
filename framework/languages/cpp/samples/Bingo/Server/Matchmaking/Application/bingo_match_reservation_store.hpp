/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/Contracts/messages.hpp"

namespace zlink::samples::bingo
{

class bingo_match_reservation_store_t
{
  public:
    virtual ~bingo_match_reservation_store_t () = default;

    virtual reserve_bingo_room_res_t
    reserve (const reserve_bingo_room_req_t &request) = 0;
};

} // namespace zlink::samples::bingo
