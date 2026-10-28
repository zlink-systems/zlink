/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <string_view>
#include <tuple>

namespace zlink::json
{
template <typename T> struct dto_traits;
template <typename E> struct enum_traits;

template <typename C, typename M> struct field_descriptor
{
    std::string_view name;
    M C::*member;
};
template <typename C, typename M> constexpr auto field (std::string_view name, M C::*member)
{
    return field_descriptor<C, M>{name, member};
}
template <typename E> struct enum_descriptor
{
    E value;
    std::string_view name;
};
template <typename E> constexpr auto enum_value (E value, std::string_view name)
{
    return enum_descriptor<E>{value, name};
}
} // namespace zlink::json