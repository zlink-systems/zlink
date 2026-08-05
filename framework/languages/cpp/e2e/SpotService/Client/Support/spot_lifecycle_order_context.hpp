/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <string>

namespace zlink::framework::e2e::spot_service::client
{

struct spot_lifecycle_order_context_t
{
    std::string key = "spot-owner-order-sm-a4";
    std::string spot_id = user_spot_id_for_key (key);
    int current_value = 0;
};

} // namespace zlink::framework::e2e::spot_service::client
