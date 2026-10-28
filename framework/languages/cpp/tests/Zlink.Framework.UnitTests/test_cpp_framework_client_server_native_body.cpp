/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/backend/raw_dealer_port.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/messaging/envelope_codec.hpp"

#include <cassert>
#include <cstring>
#include <type_traits>

using namespace zlink::framework;

int main ()
{
    // Compile-time coverage ensures the backend accepts native parts directly.
    using parts_t = std::vector<zlink::message_t>;
    using send_t = task_t<zlink::submit_result_t> (detail::backend::raw_dealer_port_t::*) (parts_t);
    using reply_t = bool (detail::backend::raw_route_port_t::*) (
      const detail::backend::raw_received_t &, parts_t);
    [[maybe_unused]] send_t send = &detail::backend::raw_dealer_port_t::send_result;
    [[maybe_unused]] reply_t reply = &detail::backend::raw_route_port_t::reply;
    static_assert (std::is_same_v<
                   decltype (runtime::client_server::client_server_request_completion_t{}.payload),
                   zlink::message_t>);

    // A body larger than the binding's inline storage keeps its buffer through both
    // envelope ownership transfer and request completion ownership transfer.
    zlink::message_t body (128 * 1024);
    std::memset (body.data (), 0x5a, body.size ());
    const void *address = body.data ();
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::response;
    header.channel_name = "native-body";
    header.message_name = "native.reply";
    auto parts = std::move (runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (
                              header, std::move (body)))
                   .take_items ();
    assert (parts.size () == 2);
    assert (parts[1].data () == address);
    runtime::client_server::client_server_request_completion_t completion;
    completion.payload = std::move (parts[1]);
    assert (completion.payload.data () == address);
    assert (completion.payload.size () == 128 * 1024);
    for (std::size_t i = 0; i < completion.payload.size (); ++i)
        assert (reinterpret_cast<const unsigned char *> (completion.payload.data ())[i] == 0x5a);
}
