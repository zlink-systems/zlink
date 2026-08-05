/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>

namespace zlink::framework
{

enum class transport_scheme_t
{
    tcp = 0,
    ipc = 1,
    tls = 2,
    websocket = 3,
    websocket_tls = 4
};

class transport_endpoint_t
{
  public:
    transport_endpoint_t (transport_scheme_t scheme, std::string uri);

    transport_scheme_t scheme () const noexcept { return _scheme; }
    const std::string &uri () const noexcept { return _uri; }

    static transport_endpoint_t parse (std::string uri);

  private:
    transport_scheme_t _scheme;
    std::string _uri;
};

} // namespace zlink::framework
