/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/http_client/contracts/coroutines.hpp>
#include <zlink/http_client/contracts/types.hpp>
#include <zlink/framework/codecs/json.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::http_client
{

class request_builder_t;
class server_request_builder_t;
class client_builder_t;
class server_client_t;
template <typename TName> class named_server_client_t;

class execution_turn_t
{
  public:
    virtual ~execution_turn_t () = default;
    virtual zlink::framework::detail::task_scheduler_t prepare (bool release_turn) = 0;
    virtual zlink::framework::detail::task_scheduler_t callback_scheduler () = 0;
};

class framework_execution_turn_t final : public execution_turn_t
{
  public:
    zlink::framework::detail::task_scheduler_t prepare (bool release_turn) override
    {
        auto plan = zlink::framework::detail::prepare_serial_turn_await (release_turn);
        return plan ? std::move (plan->scheduler) : zlink::framework::detail::task_scheduler_t{};
    }

    zlink::framework::detail::task_scheduler_t callback_scheduler () override
    {
        auto turn = zlink::framework::detail::capture_current_serial_turn ();
        return turn ? turn->resume_scheduler () : zlink::framework::detail::task_scheduler_t{};
    }
};

namespace detail
{
class http_client_runtime_t;
struct http_request_t;
}

class client_t
{
  public:
    client_t () = default;

    static client_builder_t create ();
    static client_builder_t create (std::string base_url);

    request_builder_t get (std::string path) const;
    request_builder_t post (std::string path) const;
    request_builder_t put (std::string path) const;
    request_builder_t delete_ (std::string path) const;
    request_builder_t patch (std::string path) const;
    request_builder_t head (std::string path) const;
    request_builder_t options (std::string path) const;

  private:
    explicit client_t (std::shared_ptr<detail::http_client_runtime_t> runtime);

    std::shared_ptr<detail::http_client_runtime_t> _runtime;

    friend class client_builder_t;
    friend class request_builder_t;
};

class client_builder_t
{
  public:
    client_builder_t &base_url (std::string value);
    client_builder_t &timeout (std::chrono::milliseconds value);
    client_builder_t &default_header (std::string name, std::string value);
    client_builder_t &basic_auth (const std::string &user, const std::string &password);
    client_builder_t &bearer_token (const std::string &token);
    client_builder_t &max_response_body_size (std::size_t bytes);
    client_builder_t &trust_certificate_file (std::string path);
    client_builder_t &client_certificate_file (std::string certificate_path, std::string key_path);
    client_builder_t &follow_redirects (int max_redirects = 5);
    client_builder_t &retry (int attempts);
    client_builder_t &cookies ();
    client_builder_t &proxy (std::string url);
    client_builder_t &proxy_basic_auth (const std::string &user, const std::string &password);
    client_builder_t &compression ();
    client_builder_t &coroutines ();
    client_builder_t &coroutines (std::shared_ptr<coroutine_resume_scheduler_t> resume_scheduler);
    client_builder_t &coroutines (std::shared_ptr<coroutine_execute_scheduler_t> execute_scheduler,
                                  std::shared_ptr<coroutine_resume_scheduler_t> resume_scheduler);

    client_t build () const;
    server_client_t build_server (std::shared_ptr<execution_turn_t> execution_turn) const;
    template <typename TName>
    named_server_client_t<TName>
    build_server (std::shared_ptr<execution_turn_t> execution_turn) const;

    // One-shot shortcuts: build the client on demand so `build()` can be
    // omitted for single requests. The returned request owns its client
    // (see request_builder_t::_client), so the runtime stays alive for the
    // duration of the request even when the builder is a temporary.
    request_builder_t get (std::string path) const;
    request_builder_t post (std::string path) const;
    request_builder_t put (std::string path) const;
    request_builder_t delete_ (std::string path) const;
    request_builder_t patch (std::string path) const;
    request_builder_t head (std::string path) const;
    request_builder_t options (std::string path) const;

  private:
    std::string _base_url;
    std::chrono::milliseconds _timeout{3000};
    std::size_t _max_response_body_size = 16 * 1024 * 1024;
    std::map<std::string, std::string> _headers;
    std::optional<std::string> _trust_certificate_file;
    std::optional<std::pair<std::string, std::string>> _client_certificate;
    int _follow_redirects = 0;
    int _retry_attempts = 0;
    bool _cookies = false;
    std::optional<std::string> _proxy;
    std::optional<std::string> _proxy_authorization;
    bool _compression = false;
    bool _coroutines = true;
    std::shared_ptr<coroutine_execute_scheduler_t> _execute_scheduler;
    std::shared_ptr<coroutine_resume_scheduler_t> _resume_scheduler;
};

class request_builder_t
{
  public:
    using body_stream_provider_t = std::function<std::optional<std::string> ()>;

    request_builder_t (client_t client, http_method_t method, std::string path);

    request_builder_t &header (std::string name, std::string value);
    request_builder_t &query (std::string name, std::string value);
    request_builder_t &timeout (std::chrono::milliseconds value);

    template <typename T> request_builder_t &body (const T &value)
    {
        _body = zlink::message_t::from_json (value).to_string ();
        _headers.try_emplace ("content-type", "application/json");
        return *this;
    }

    request_builder_t &body (std::string content, std::string content_type);

    // Streams the request body chunk by chunk with chunked transfer-encoding;
    // the provider returns std::nullopt when the body is complete. Requests
    // with a streamed body are sent on a fresh connection and are excluded
    // from automatic retry (the provider cannot be rewound).
    request_builder_t &body_stream (body_stream_provider_t provider, std::string content_type);

    request_builder_t &form (std::string name, std::string value);
    request_builder_t &multipart (std::string name, std::string value);
    request_builder_t &multipart_file (std::string name,
                                       std::string filename,
                                       std::string content,
                                       std::string content_type);

    zlink::framework::task_t<raw_http_response_t> submit_raw () const;

    // Streams the response body to `sink` chunk by chunk instead of buffering
    // it; the returned response carries status and headers with an empty body.
    // Chunks are delivered as received (no content-encoding decompression).
    zlink::framework::task_t<raw_http_response_t>
    download (std::function<void (std::string_view)> sink) const;

    template <typename T> zlink::framework::task_t<http_response_t<T>> submit () const
    {
        auto raw_task = submit_raw ();
        raw_http_response_t raw;
        try {
            raw = co_await raw_task;
        }
        catch (const zlink::framework::framework_exception_t &error) {
            co_return zlink::framework::result_t<http_response_t<T>>::failure (
              error.kind (), error.what ());
        }

        if (raw.status >= 400) {
            std::ostringstream message;
            message << "HTTP request failed with status " << raw.status;
            if (!raw.body.empty ()) {
                constexpr std::size_t max_error_body = 512;
                message << ": " << raw.body.substr (0, max_error_body);
                if (raw.body.size () > max_error_body)
                    message << "...";
            }
            co_return zlink::framework::result_t<http_response_t<T>>::failure (
              zlink::framework::framework_error_kind_t::internal_failure, message.str ());
        }

        try {
            http_response_t<T> response{
              .status = raw.status,
              .headers = raw.headers,
              .body = zlink::message_t::from (raw.body).template parse_json<T> (),
              .raw_body = raw.body};
            co_return response;
        }
        catch (const std::exception &ex) {
            co_return zlink::framework::result_t<http_response_t<T>>::failure (
              zlink::framework::framework_error_kind_t::protocol_error, ex.what ());
        }
    }

    template <typename T> T fetch () const
    {
        auto response = submit<T> ().result ().value ();
        return std::move (response.body);
    }

    template <typename T, typename TCallback> void submit (TCallback &&callback) const
    {
        auto task = submit<T> ();
        zlink::framework::detail::observe_task_completion (task,
                                                           std::forward<TCallback> (callback));
    }

  private:
    struct multipart_part_t
    {
        std::string name;
        std::string filename;
        std::string content;
        std::string content_type;
    };

    std::string resolve_target () const;
    std::pair<std::optional<std::string>, std::map<std::string, std::string>>
    resolve_body_and_headers () const;
    detail::http_request_t make_request (std::function<void (std::string_view)> sink) const;
    zlink::framework::task_t<raw_http_response_t> dispatch_request (detail::http_request_t request) const;

    client_t _client;
    http_method_t _method;
    std::string _path;
    std::optional<std::string> _body;
    std::function<std::optional<std::string> ()> _body_provider;
    std::map<std::string, std::string> _headers;
    std::optional<std::chrono::milliseconds> _timeout;
    std::vector<std::pair<std::string, std::string>> _query;
    std::vector<std::pair<std::string, std::string>> _form;
    std::vector<multipart_part_t> _multipart;
};

class server_request_builder_t : public request_builder_t
{
  public:
    server_request_builder_t (client_t client,
                              http_method_t method,
                              std::string path,
                              std::shared_ptr<execution_turn_t> execution_turn) :
        request_builder_t (std::move (client), method, std::move (path)),
        _execution_turn (std::move (execution_turn))
    {
    }

    server_request_builder_t &header (std::string name, std::string value)
    {
        request_builder_t::header (std::move (name), std::move (value));
        return *this;
    }
    server_request_builder_t &query (std::string name, std::string value)
    {
        request_builder_t::query (std::move (name), std::move (value));
        return *this;
    }
    server_request_builder_t &timeout (std::chrono::milliseconds value)
    {
        request_builder_t::timeout (value);
        return *this;
    }
    template <typename T> server_request_builder_t &body (const T &value)
    {
        request_builder_t::body (value);
        return *this;
    }
    server_request_builder_t &body (std::string content, std::string content_type)
    {
        request_builder_t::body (std::move (content), std::move (content_type));
        return *this;
    }
    server_request_builder_t &body_stream (body_stream_provider_t provider,
                                           std::string content_type)
    {
        request_builder_t::body_stream (std::move (provider), std::move (content_type));
        return *this;
    }
    server_request_builder_t &form (std::string name, std::string value)
    {
        request_builder_t::form (std::move (name), std::move (value));
        return *this;
    }
    server_request_builder_t &multipart (std::string name, std::string value)
    {
        request_builder_t::multipart (std::move (name), std::move (value));
        return *this;
    }
    server_request_builder_t &multipart_file (std::string name,
                                              std::string filename,
                                              std::string content,
                                              std::string content_type)
    {
        request_builder_t::multipart_file (std::move (name), std::move (filename),
                                           std::move (content), std::move (content_type));
        return *this;
    }

    zlink::framework::task_t<void> submit () const
    {
        (void) co_await schedule_raw (false);
        co_return;
    }

    template <typename T> zlink::framework::task_t<http_response_t<T>> submit () const
    {
        return schedule<T> (false);
    }

    zlink::framework::task_t<raw_http_response_t> submit_raw () const
    {
        return schedule_raw (false);
    }

    template <typename T> zlink::framework::task_t<http_response_t<T>> yield () const
    {
        return schedule<T> (true);
    }

    zlink::framework::task_t<raw_http_response_t> yield_raw () const
    {
        return schedule_raw (true);
    }

    template <typename T, typename TCallback> void submit (TCallback &&callback) const
    {
        auto task = request_builder_t::submit<T> ();
        auto scheduler = _execution_turn->callback_scheduler ();
        if (scheduler) {
            task = zlink::framework::detail::reschedule_task (std::move (task),
                                                              std::move (scheduler));
        }
        zlink::framework::detail::observe_task_completion (task,
                                                           std::forward<TCallback> (callback));
    }

  private:
    template <typename T>
    zlink::framework::task_t<http_response_t<T>> schedule (bool release_turn) const
    {
        auto task = request_builder_t::submit<T> ();
        auto scheduler = _execution_turn->prepare (release_turn);
        if (!scheduler) {
            return task;
        }
        return zlink::framework::detail::reschedule_task (std::move (task),
                                                          std::move (scheduler));
    }

    zlink::framework::task_t<raw_http_response_t> schedule_raw (bool release_turn) const
    {
        auto task = request_builder_t::submit_raw ();
        auto scheduler = _execution_turn->prepare (release_turn);
        if (!scheduler) {
            return task;
        }
        return zlink::framework::detail::reschedule_task (std::move (task),
                                                          std::move (scheduler));
    }

    std::shared_ptr<execution_turn_t> _execution_turn;
};

class server_client_t
{
  public:
    server_request_builder_t get (std::string path) const;
    server_request_builder_t post (std::string path) const;
    server_request_builder_t put (std::string path) const;
    server_request_builder_t delete_ (std::string path) const;
    server_request_builder_t patch (std::string path) const;
    server_request_builder_t head (std::string path) const;
    server_request_builder_t options (std::string path) const;

  private:
    server_client_t (client_t client, std::shared_ptr<execution_turn_t> execution_turn) :
        _client (std::move (client)), _execution_turn (std::move (execution_turn))
    {
    }

    client_t _client;
    std::shared_ptr<execution_turn_t> _execution_turn;
    friend class client_builder_t;
};

template <typename TName> class named_server_client_t : public server_client_t
{
  public:
    explicit named_server_client_t (server_client_t client) : server_client_t (std::move (client)) {}
};

template <typename TName>
named_server_client_t<TName>
client_builder_t::build_server (std::shared_ptr<execution_turn_t> execution_turn) const
{
    return named_server_client_t<TName> (build_server (std::move (execution_turn)));
}

} // namespace zlink::http_client
