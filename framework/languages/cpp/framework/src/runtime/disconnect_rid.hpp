/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/Contracts/Errors/errors.hpp>

#include <utility>

namespace zlink::framework::runtime
{

template <typename Disconnect> void disconnect_rid_if_present (Disconnect &&disconnect)
{
    try {
        std::forward<Disconnect> (disconnect) ();
    }
    catch (const zlink::connect_error_t &error) {
        if (error.result () != zlink::connect_result_t::not_found)
            throw;
    }
}

} // namespace zlink::framework::runtime
