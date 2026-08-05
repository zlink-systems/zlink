/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "runtime/http_client_runtime.hpp"

namespace zlink::http_client::detail
{

class retry_policy_t
{
  public:
    retry_policy_t (const http_client_options_t &options, const http_request_t &request);

    bool should_retry (int attempt,
                       const zlink::framework::result_t<raw_http_response_t> &result) const;
    static std::chrono::milliseconds delay (int attempt);

  private:
    int _max_retries;
};

} // namespace zlink::http_client::detail
