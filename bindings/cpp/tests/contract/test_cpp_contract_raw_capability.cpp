/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink.hpp>

#include <functional>
#include <type_traits>
#include <utility>

namespace
{

using pair_send_t = decltype (std::declval<zlink::pair_socket_t &> ().send ());
using multipart_submit_t = decltype (
  std::declval<pair_send_t> ()
    .message (std::declval<zlink::message_t &> ())
    .message (std::declval<zlink::message_t &> ())
    .submit ());

static_assert (std::is_same<multipart_submit_t, bool>::value,
               "the public C++ package must expose multipart send");
static_assert (std::is_same<decltype (std::declval<zlink::received_t &> ().parts ()),
                            std::vector<zlink::message_t> &>::value,
               "the public C++ package must expose multipart receive");

static_assert (
  std::is_same<decltype (std::declval<zlink::socket_t &> ().monitor_open ()),
               zlink::socket_monitor_t>::value,
  "the public C++ package must expose raw socket monitoring");
static_assert (
  std::is_same<decltype (std::declval<zlink::socket_monitor_t &> ().status ()),
               zlink::monitor_status_t>::value,
  "the public C++ package must expose monitor status snapshots");
static_assert (
  std::is_same<decltype (std::declval<const zlink::monitor_status_t &> ().is_ready ()),
               bool>::value,
  "the public C++ package must expose socket readiness");

static_assert (
  std::is_same<decltype (std::declval<zlink::stream_socket_t &> ().send (
                              std::declval<const zlink::routing_id_t &> ())),
               zlink::send_operation_t>::value,
  "the public C++ package must expose routed STREAM send");
static_assert (
  std::is_same<decltype (std::declval<zlink::stream_socket_t &> ().recv (
                              std::declval<zlink::received_t &> ())),
               int>::value,
  "the public C++ package must expose STREAM receive");

using shutdown_t = decltype (std::declval<zlink::context_t &> ().shutdown ());
using socket_close_t = decltype (std::declval<zlink::socket_t &> ().close ());
using monitor_close_t = decltype (std::declval<zlink::socket_monitor_t &> ().close ());
static_assert (std::is_same<shutdown_t, void>::value
                 && std::is_same<socket_close_t, void>::value
                 && std::is_same<monitor_close_t, void>::value,
               "the public C++ package must expose orderly shutdown");

} // namespace

int main ()
{
    return 0;
}
