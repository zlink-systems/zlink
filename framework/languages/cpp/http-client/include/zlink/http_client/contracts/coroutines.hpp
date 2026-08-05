/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/framework/contracts/errors/result.hpp>

#include <coroutine>
#include <functional>
#include <utility>

namespace zlink::http_client
{

class coroutine_execute_scheduler_t
{
  public:
    virtual ~coroutine_execute_scheduler_t () = default;

    virtual void execute (std::function<void ()> work) = 0;
};

class coroutine_resume_scheduler_t
{
  public:
    virtual ~coroutine_resume_scheduler_t () = default;

    virtual void resume (std::function<void ()> continuation) = 0;
    virtual void resume (std::coroutine_handle<> continuation)
    {
        resume ([continuation] { continuation.resume (); });
    }
};

class framework_resume_scheduler_t final : public coroutine_resume_scheduler_t
{
  public:
    using post_t = std::function<void (std::function<void ()>)>;

    explicit framework_resume_scheduler_t (post_t post) : _post (std::move (post))
    {
        if (!_post) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::protocol_error,
              "HTTP client framework resume scheduler post function is required");
        }
    }

    void resume (std::function<void ()> continuation) override { _post (std::move (continuation)); }

  private:
    post_t _post;
};

} // namespace zlink::http_client
