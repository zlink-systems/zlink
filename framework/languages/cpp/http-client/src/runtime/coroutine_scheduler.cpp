/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/http_client_runtime.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>

namespace zlink::http_client::detail
{
namespace
{
namespace asio = boost::asio;

class default_coroutine_scheduler_t final
    : public zlink::http_client::coroutine_execute_scheduler_t,
      public zlink::http_client::coroutine_resume_scheduler_t
{
  public:
    //  Execute runs blocking Beast exchanges, so it gets a small pool (4, matching the
    //  framework I/O thread convention) instead of a single shared thread that serialized
    //  every request. Resume runs on its own thread so a continuation that blocks on
    //  another HTTP task cannot occupy the only worker and deadlock against execute.
    default_coroutine_scheduler_t () : _execute_pool (4), _resume_pool (1) {}

    void execute (std::function<void ()> work) override
    {
        asio::post (_execute_pool, [work = std::move (work)] () mutable { work (); });
    }

    void resume (std::function<void ()> continuation) override
    {
        asio::post (_resume_pool, [continuation = std::move (continuation)] () mutable {
            try {
                continuation ();
            }
            catch (...) {
                //  A continuation exception has no owner to report to on this detached
                //  thread; swallowing keeps the resume thread alive for later tasks.
            }
        });
    }

  private:
    asio::thread_pool _execute_pool;
    asio::thread_pool _resume_pool;
};

std::shared_ptr<default_coroutine_scheduler_t> default_scheduler_instance ()
{
    static auto *scheduler = new std::shared_ptr<default_coroutine_scheduler_t> (
      std::make_shared<default_coroutine_scheduler_t> ());
    return *scheduler;
}

} // namespace

std::shared_ptr<coroutine_execute_scheduler_t> default_coroutine_execute_scheduler ()
{
    return default_scheduler_instance ();
}

std::shared_ptr<coroutine_resume_scheduler_t> default_coroutine_resume_scheduler ()
{
    return default_scheduler_instance ();
}

} // namespace zlink::http_client::detail
