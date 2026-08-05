/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client
{

inline void ensure (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

} // namespace zlink::framework::e2e::spot_service::client
