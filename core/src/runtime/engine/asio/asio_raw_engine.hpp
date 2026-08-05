/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_RAW_ENGINE_HPP_INCLUDED__
#define __ZLINK_ASIO_RAW_ENGINE_HPP_INCLUDED__

#include "engine/asio/asio_engine.hpp"
#if defined ZLINK_HAVE_ASIO_SSL
#include <boost/asio/ssl.hpp>
#endif

namespace zlink
{
class asio_raw_engine_t ZLINK_FINAL : public asio_engine_t
{
  public:
    asio_raw_engine_t (fd_t fd_,
                       const options_t &options_,
                       const endpoint_uri_pair_t &endpoint_uri_pair_);
    asio_raw_engine_t (fd_t fd_,
                       const options_t &options_,
                       const endpoint_uri_pair_t &endpoint_uri_pair_,
                       std::unique_ptr<i_asio_transport> transport_);
#if defined ZLINK_HAVE_ASIO_SSL
    asio_raw_engine_t (fd_t fd_,
                       const options_t &options_,
                       const endpoint_uri_pair_t &endpoint_uri_pair_,
                       std::unique_ptr<i_asio_transport> transport_,
                       std::unique_ptr<boost::asio::ssl::context> ssl_context_);
#endif
    ~asio_raw_engine_t () ZLINK_OVERRIDE;

  protected:
    void plug_internal () ZLINK_OVERRIDE;
    bool build_gather_header (const msg_t &msg_,
                              unsigned char *buffer_,
                              size_t buffer_size_,
                              size_t &header_size_) ZLINK_OVERRIDE;

  private:
    void init_raw_engine ();

#if defined ZLINK_HAVE_ASIO_SSL
    std::unique_ptr<boost::asio::ssl::context> _ssl_context;
#endif

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_raw_engine_t)
};
}

#endif
