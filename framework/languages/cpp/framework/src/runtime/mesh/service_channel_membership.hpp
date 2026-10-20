/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/mesh/service_topology_registry.hpp"
#include <algorithm>
#include <memory>
#include <string>

namespace zlink::framework::runtime::mesh
{

template <typename Descriptor>
auto find_service_channel (Descriptor &descriptor, const std::string &channel_name)
{
    const auto found =
      std::lower_bound (descriptor.channels.begin (), descriptor.channels.end (), channel_name,
                        [] (const service_channel_descriptor_t &channel, const std::string &name) {
                            return channel.name < name;
                        });
    return found != descriptor.channels.end () && found->name == channel_name
             ? std::addressof (*found)
             : nullptr;
}

} // namespace zlink::framework::runtime::mesh
