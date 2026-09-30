/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/contracts/throwing_result.hpp>

#include <utility>

namespace zlink::stream_connector_throwing
{

class connector_t
{
  public:
    explicit connector_t (zlink::stream_connector::connector_t connector) :
        _connector (std::move (connector))
    {
    }

    void connect () { value_or_throw (_connector.connect ()); }
    void close () { value_or_throw (_connector.close ()); }
    zlink::stream_connector::connector_t &core () noexcept { return _connector; }
    const zlink::stream_connector::connector_t &core () const noexcept { return _connector; }

  private:
    zlink::stream_connector::connector_t _connector;
};

inline connector_t create (zlink::stream_connector::connector_options_t options)
{
    return connector_t (zlink::stream_connector::connector_factory_t::create (std::move (options)));
}

} // namespace zlink::stream_connector_throwing
