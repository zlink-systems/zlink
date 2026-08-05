/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../../Shared/automatic_turn_dispatch_contracts.hpp"

#include <zlink/framework.hpp>

namespace zlink::framework::e2e::automatic_turn_dispatch::server
{

inline void configure_codecs (zlink::framework::codec_options_builder_t codecs)
{
    namespace yd = zlink::framework::e2e::automatic_turn_dispatch;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server
