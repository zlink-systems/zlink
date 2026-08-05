/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/http_client/contracts/client.hpp>

#include "runtime/cookie_jar.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace zlink::http_client::detail
{

struct http_client_options_t
{
    std::string base_url;
    std::chrono::milliseconds timeout{3000};
    std::uint64_t max_response_body_size = 16 * 1024 * 1024;
    std::map<std::string, std::string> headers;
    std::optional<std::string> trust_certificate_file;
    std::optional<std::pair<std::string, std::string>> client_certificate;
    int follow_redirects = 0;
    int retry_attempts = 0;
    bool cookies = false;
    std::optional<std::string> proxy;
    std::optional<std::string> proxy_authorization;
    bool compression = false;
    bool coroutines = false;
    std::shared_ptr<coroutine_execute_scheduler_t> execute_scheduler;
    std::shared_ptr<coroutine_resume_scheduler_t> resume_scheduler;
};

struct http_request_t
{
    http_method_t method;
    std::string path;
    std::optional<std::string> body;
    std::function<std::optional<std::string> ()> body_provider;
    std::map<std::string, std::string> headers;
    std::optional<std::chrono::milliseconds> timeout;
    std::function<void (std::string_view)> sink;
};

class connection_pool_t;

class http_client_runtime_t : public std::enable_shared_from_this<http_client_runtime_t>
{
  public:
    explicit http_client_runtime_t (http_client_options_t options);
    ~http_client_runtime_t ();

    zlink::framework::result_t<raw_http_response_t> execute (const http_request_t &request) const;
    zlink::framework::task_t<raw_http_response_t> submit (http_request_t request) const;
    bool uses_coroutines () const;
    zlink::framework::detail::task_scheduler_t completion_scheduler () const;

  private:
    zlink::framework::result_t<raw_http_response_t>
    execute_with_deadline (http_request_t request,
                           std::chrono::steady_clock::time_point deadline) const;

    http_client_options_t _options;
    mutable cookie_jar_t _cookie_jar;
    std::unique_ptr<connection_pool_t> _connection_pool;
};

std::shared_ptr<coroutine_execute_scheduler_t> default_coroutine_execute_scheduler ();
std::shared_ptr<coroutine_resume_scheduler_t> default_coroutine_resume_scheduler ();

} // namespace zlink::http_client::detail
