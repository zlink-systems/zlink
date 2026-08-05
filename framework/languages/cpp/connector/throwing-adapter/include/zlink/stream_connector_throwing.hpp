/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace zlink::stream_connector_throwing
{

class stream_connector_error : public std::runtime_error
{
  public:
    stream_connector_error (zlink::stream_connector::error_code_t code, std::string message) :
        std::runtime_error (message),
        _code (code)
    {
    }

    zlink::stream_connector::error_code_t code () const noexcept { return _code; }

  private:
    zlink::stream_connector::error_code_t _code;
};

template <typename T> T value_or_throw (zlink::stream_connector::result_t<T> result)
{
    if (result) {
        return std::move (result.value ());
    }
    const auto &error = result.error ();
    throw stream_connector_error (result.error_code (),
                                  error ? error->message : "stream connector operation failed");
}

inline void value_or_throw (zlink::stream_connector::result_t<void> result)
{
    if (result) {
        return;
    }
    const auto &error = result.error ();
    throw stream_connector_error (result.error_code (),
                                  error ? error->message : "stream connector operation failed");
}

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
