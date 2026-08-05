/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>

#ifdef ZLINK_HTTP_CLIENT_WITH_OPENSSL
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#endif

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zlink::http_client::detail
{

struct pooled_connection_t
{
    boost::asio::io_context io;
    std::optional<boost::beast::tcp_stream> plain;
#ifdef ZLINK_HTTP_CLIENT_WITH_OPENSSL
    std::optional<boost::asio::ssl::context> tls_context;
    std::optional<boost::asio::ssl::stream<boost::beast::tcp_stream>> secure;
#endif
    boost::beast::flat_buffer buffer;
};

class connection_pool_t
{
  public:
    std::unique_ptr<pooled_connection_t> acquire (const std::string &key);
    void release (const std::string &key, std::unique_ptr<pooled_connection_t> connection);

  private:
    struct idle_entry_t
    {
        std::unique_ptr<pooled_connection_t> connection;
        std::chrono::steady_clock::time_point released_at;
    };

    std::mutex _mutex;
    std::map<std::string, std::vector<idle_entry_t>> _idle;
};

} // namespace zlink::http_client::detail
