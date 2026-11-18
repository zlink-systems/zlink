/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "../bingo_room_spot.hpp"

namespace zlink::samples::bingo
{

inline task_t<void> bingo_room_close_timer_handler_t::handle (bingo_room_spot_t &spot,
                                                              const timer_tick_t &) const
{
    if (!spot.try_request_close ())
        co_return;
    std::cout << "bingo-lifecycle room-close spot=" << spot.context ().spot_id () << std::endl;
    (void) spot.context ().close ();
    co_return;
}

} // namespace zlink::samples::bingo
