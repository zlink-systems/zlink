/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

namespace zlink::framework
{

namespace detail
{
struct request_task_access_t;
// Carries source-local acceptance to the public admission observer.
struct request_admitted_t
{
};
struct request_admission_source_t
{
};

template <typename T> std::exception_ptr request_failure (const result_t<T> &result)
{
    return result.exception ();
}

template <typename T> std::exception_ptr request_failure (const result_t<result_t<T>> &result)
{
    return result ? request_failure (result.value ()) : result.exception ();
}

template <typename T>
void observe_request_failure (task_t<T> &reply,
                              const std::shared_ptr<task_completion_source_t<void>> &admitted)
{
    detail::observe_task_terminal (reply, [admitted] (const auto &result) {
        if (auto failure = request_failure (result))
            admitted->complete (result_access_t::failure<void> (std::move (failure)));
    });
}

}

template <typename TReply> class request_task_t : public task_t<TReply>
{
  public:
    struct promise_type : task_t<TReply>::promise_type
    {
        request_task_t get_return_object ()
        {
            return request_task_t (task_t<TReply>::promise_type::get_return_object (), _admitted);
        }

        std::suspend_never await_transform (detail::request_admitted_t)
        {
            _admitted->complete (result_t<void>::success ());
            return {};
        }

        auto await_transform (detail::request_admission_source_t)
        {
            struct source_awaiter_t
            {
                std::shared_ptr<task_completion_source_t<void>> source;
                bool await_ready () const noexcept { return true; }
                void await_suspend (std::coroutine_handle<>) const noexcept {}
                auto await_resume () const { return source; }
            };
            return source_awaiter_t{_admitted};
        }

        template <typename T> task_t<T> await_transform (request_task_t<T> &pending)
        {
            forward_admission (pending);
            return pending.physical_reply ();
        }

        template <typename T> task_t<T> await_transform (request_task_t<T> &&pending)
        {
            forward_admission (pending);
            return pending.physical_reply ();
        }

        template <typename T> decltype (auto) await_transform (T &&pending)
        {
            return std::forward<T> (pending);
        }

      private:
        template <typename T> void forward_admission (request_task_t<T> &pending)
        {
            auto admitted = pending.admitted ();
            detail::observe_task_terminal (
              admitted, [source = _admitted] (const auto &result) { source->complete (result); });
        }

        std::shared_ptr<task_completion_source_t<void>> _admitted =
          std::make_shared<task_completion_source_t<void>> ();
    };

    request_task_t (result_t<TReply> result) :
        request_task_t (task_t<TReply> (result),
                        std::make_shared<task_completion_source_t<void>> ())
    {
        if (result)
            _admitted->complete (result_t<void>::success ());
    }

    request_task_t (task_t<TReply> reply,
                    std::shared_ptr<task_completion_source_t<void>> admitted,
                    detail::task_scheduler_t scheduler = {}) :
        task_t<TReply> (std::make_shared<detail::task_shared_state_t<TReply>> ()),
        _physical_reply (std::move (reply)),
        _admitted (std::move (admitted)),
        _admission_scheduler (std::move (scheduler))
    {
        // Public failure may precede the physical terminal. Keep the producer's
        // task intact so internal consumers retain their coroutine parameters.
        detail::observe_task_terminal (
          _physical_reply, [reply = this->_state, admitted = _admitted] (const auto &result) {
              if (auto failure = detail::request_failure (result))
                  admitted->complete (detail::result_access_t::failure<void> (std::move (failure)));
              reply->complete (result);
          });
        auto stage = _admitted->task ();
        detail::observe_task_terminal (stage, [reply = this->_state] (const auto &result) {
            if (!result)
                reply->complete (detail::result_access_t::failure<TReply> (result.exception ()));
        });
    }

    request_task_t (request_task_t reply,
                    std::shared_ptr<task_completion_source_t<void>> admitted) :
        request_task_t (reply.physical_reply (), std::move (admitted))
    {
    }

    task_t<void> admitted ()
    {
        return detail::with_task_resume_scheduler (_admitted->task (), _admission_scheduler);
    }

  private:
    friend struct detail::request_task_access_t;
    template <typename> friend class request_task_t;

    task_t<TReply> physical_reply () const
    {
        return detail::with_task_resume_scheduler (task_t<TReply> (_physical_reply._state),
                                                   _physical_reply._resume_scheduler
                                                     ? *_physical_reply._resume_scheduler
                                                     : detail::task_scheduler_t{});
    }

    task_t<TReply> _physical_reply;
    std::shared_ptr<task_completion_source_t<void>> _admitted;
    detail::task_scheduler_t _admission_scheduler;
};

namespace detail
{
struct request_task_access_t
{
    template <typename T> static auto physical_reply (const request_task_t<T> &task)
    {
        return task.physical_reply ();
    }

    template <typename T> static auto admission_source (const request_task_t<T> &task)
    {
        return task._admitted;
    }

    template <typename T>
    static request_task_t<T> reschedule (request_task_t<T> task, task_scheduler_t scheduler)
    {
        static_cast<task_t<T> &> (task) =
          with_task_resume_scheduler (std::move (static_cast<task_t<T> &> (task)), scheduler);
        task._physical_reply =
          with_task_resume_scheduler (std::move (task._physical_reply), scheduler);
        task._admission_scheduler = std::move (scheduler);
        return task;
    }
};

template <typename T>
request_task_t<T> reschedule_task (request_task_t<T> pending, task_scheduler_t scheduler)
{
    return request_task_access_t::reschedule (std::move (pending), std::move (scheduler));
}
}

} // namespace zlink::framework
