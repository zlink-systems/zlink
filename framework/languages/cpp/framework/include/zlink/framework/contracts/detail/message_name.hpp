/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <concepts>
#include <string>
#include <typeinfo>

namespace zlink::framework::detail
{

template <typename T> concept static_packet_name = requires
{
    {
        T::packet_name
    } -> std::convertible_to<const char *>;
};

template <typename T> std::string message_name ()
{
    if constexpr (static_packet_name<T>) {
        return T::packet_name;
    } else {
        return typeid (T).name ();
    }
}

} // namespace zlink::framework::detail
