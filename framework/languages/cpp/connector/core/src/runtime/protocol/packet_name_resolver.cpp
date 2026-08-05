/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/packet_name_resolver.hpp"

#include <utility>

namespace zlink::stream_connector::detail
{

std::string packet_name_resolver_t::resolve (std::type_index type) const
{
    return resolve (type, {});
}

std::string packet_name_resolver_t::resolve (std::type_index type, std::string name) const
{
    if (!name.empty ()) {
        return name;
    }
    return type.name ();
}

} // namespace zlink::stream_connector::detail
