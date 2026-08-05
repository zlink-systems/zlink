/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/transport/stream_transport_factory.hpp"

namespace zlink::stream_connector::detail
{

bool stream_transport_factory_t::is_supported (transport_t transport) noexcept
{
    switch (transport) {
        case transport_t::tcp:
            return true;
        case transport_t::websocket:
#ifdef ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET
            return true;
#else
            return false;
#endif
        case transport_t::tls:
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
            return true;
#else
            return false;
#endif
        case transport_t::websocket_secure:
#if defined(ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET) && defined(ZLINK_STREAM_CONNECTOR_WITH_OPENSSL)
            return true;
#else
            return false;
#endif
    }
    return false;
}

} // namespace zlink::stream_connector::detail
