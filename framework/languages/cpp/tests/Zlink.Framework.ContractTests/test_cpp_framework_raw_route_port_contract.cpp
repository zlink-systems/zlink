/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/backend/raw_route_port.hpp"

#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>

namespace backend = zlink::framework::detail::backend;

static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_route_port_t &> ().send (
                 std::declval<const backend::raw_bytes_t &> (),
                 std::declval<const backend::raw_message_t &> ())),
               bool>);
static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_route_port_t &> ().try_receive ()),
               std::optional<backend::raw_received_t>>);
static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_route_port_t &> ().request (
                 std::declval<const backend::raw_bytes_t &> (),
                 std::declval<const backend::raw_message_t &> (),
                 std::chrono::milliseconds (1),
                 std::declval<backend::raw_route_port_t::request_callback_t> ())),
               bool>);

int main () { return 0; }
