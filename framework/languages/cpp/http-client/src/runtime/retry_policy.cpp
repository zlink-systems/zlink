/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/retry_policy.hpp"

#include <algorithm>
#include <random>

namespace zlink::http_client::detail
{

retry_policy_t::retry_policy_t (const http_client_options_t &options,
                                const http_request_t &request) :
    _max_retries ((request.sink || request.body_provider) ? 0 : options.retry_attempts)
{
}

bool retry_policy_t::should_retry (
  int attempt,
  const zlink::framework::result_t<raw_http_response_t> &result) const
{
    return !result.has_value () && attempt < _max_retries && result.error () != nullptr
           && zlink::framework::detail::is_transient_error (result.error ()->kind ());
}

std::chrono::milliseconds retry_policy_t::delay (int attempt)
{
    //  Exponential backoff with full jitter: base 50ms, doubling per attempt, capped at 1s.
    //  Fixed delays synchronize retries from many clients against an ailing server.
    const int shift = attempt < 5 ? attempt : 5;
    const long ceiling_ms = std::min<long> (1000, 50L << shift);
    thread_local std::mt19937 rng{std::random_device{} ()};
    std::uniform_int_distribution<long> jitter (0, ceiling_ms);
    return std::chrono::milliseconds (jitter (rng));
}

} // namespace zlink::http_client::detail
