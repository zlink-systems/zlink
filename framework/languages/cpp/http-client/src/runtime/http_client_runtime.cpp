/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/http_client_runtime.hpp"

#include "runtime/connection_pool.hpp"
#include "runtime/request_performer.hpp"
#include "runtime/retry_policy.hpp"
#include "runtime/runtime_errors.hpp"
#include "runtime/url.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace zlink::http_client::detail
{

http_client_runtime_t::http_client_runtime_t (http_client_options_t options) :
    _options (std::move (options)), _connection_pool (std::make_unique<connection_pool_t> ())
{
    (void) parse_base_url (_options.base_url);
    if (_options.proxy) {
        (void) parse_base_url (*_options.proxy);
    }
}

http_client_runtime_t::~http_client_runtime_t () = default;

zlink::framework::result_t<raw_http_response_t>
http_client_runtime_t::execute (const http_request_t &request) const
{
    //  The synchronous path enforces the same total wall-clock budget across retries as the
    //  coroutine path; without this the caller blocked for up to attempts x timeout.
    const auto timeout = request.timeout.value_or (_options.timeout);
    return execute_with_deadline (request, std::chrono::steady_clock::now () + timeout);
}

zlink::framework::result_t<raw_http_response_t>
http_client_runtime_t::execute_with_deadline (http_request_t request,
                                              std::chrono::steady_clock::time_point deadline) const
{
    const retry_policy_t retry_policy (_options, request);

    for (int attempt = 0;; ++attempt) {
        const auto now = std::chrono::steady_clock::now ();
        if (now >= deadline) {
            return timeout_before_exchange ();
        }

        //  A sub-millisecond remainder is not a budget to open another exchange on. Clamping it up
        //  to 1ms let the retry sleep wake a hair before the deadline and start one more attempt.
        const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now);
        if (remaining <= std::chrono::milliseconds::zero ()) {
            return timeout_before_exchange ();
        }

        auto attempt_request = request;
        attempt_request.timeout = remaining;
        auto result = perform_once (_options, _cookie_jar, *_connection_pool, attempt_request);
        if (!retry_policy.should_retry (attempt, result)) {
            return result;
        }

        const auto before_sleep = std::chrono::steady_clock::now ();
        if (before_sleep >= deadline) {
            return timeout_before_exchange ();
        }
        std::this_thread::sleep_until (
          std::min (deadline, before_sleep + retry_policy_t::delay (attempt)));
    }
}

zlink::framework::task_t<raw_http_response_t>
http_client_runtime_t::submit (http_request_t request) const
{
    const auto timeout = request.timeout.value_or (_options.timeout);
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    zlink::framework::detail::task_completion_source_t<raw_http_response_t> completion (
      completion_scheduler ());
    auto task = completion.task ();
    auto runtime = shared_from_this ();
    auto execute_scheduler = _options.execute_scheduler;
    if (!execute_scheduler) {
        completion.complete (zlink::framework::result_t<raw_http_response_t>::failure (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP client coroutine execute scheduler is not configured"));
        return task;
    }
    try {
        execute_scheduler->execute (
          [runtime, request = std::move (request), deadline, completion] () mutable {
              completion.complete (runtime->execute_with_deadline (std::move (request), deadline));
          });
    }
    catch (const std::exception &ex) {
        completion.complete (zlink::framework::detail::boundary_failure<raw_http_response_t> (zlink::framework::detail::boundary_error_t::closed,
          std::string ("HTTP client coroutine execute scheduler rejected work: ") + ex.what ()));
    }
    catch (...) {
        completion.complete (zlink::framework::detail::boundary_failure<raw_http_response_t> (zlink::framework::detail::boundary_error_t::closed,
          "HTTP client coroutine execute scheduler rejected work"));
    }
    return task;
}

bool http_client_runtime_t::uses_coroutines () const
{
    return _options.coroutines;
}

zlink::framework::detail::task_scheduler_t http_client_runtime_t::completion_scheduler () const
{
    auto resume_scheduler = _options.resume_scheduler;
    if (!resume_scheduler) {
        return {};
    }
    return [resume_scheduler = std::move (resume_scheduler)] (std::function<void ()> work) mutable {
        resume_scheduler->resume (std::move (work));
    };
}

} // namespace zlink::http_client::detail
