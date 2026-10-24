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

// Select the request_task_t constructors that project a selected request:
// for an internal layer, and for the public reply of a request call.
struct request_projection_t
{
};
struct public_request_projection_t
{
};

// This signal connects only the selected submission of the same operation.
// Ordinary nested request awaits do not publish admission to their caller.
template <typename T> struct request_submission_t
{
    task_t<T> reply;
    task_t<void> admitted;
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

        template <typename T> decltype (auto) await_transform (T &&pending)
        {
            return std::forward<T> (pending);
        }

        template <typename T> task_t<T> await_transform (detail::request_submission_t<T> submission)
        {
            detail::observe_task_terminal (
              submission.admitted,
              [source = _admitted] (const result_t<void> &result) { source->complete (result); });
            return std::move (submission.reply);
        }

      private:
        std::shared_ptr<task_completion_source_t<void>> _admitted =
          std::make_shared<task_completion_source_t<void>> ();
    };

    request_task_t (result_t<TReply> result) :
        task_t<TReply> (result),
        _physical_reply (this->_state),
        _admitted (std::make_shared<task_completion_source_t<void>> ())
    {
        const auto failure = detail::request_failure (result);
        _admitted->complete (failure ? detail::result_access_t::failure<void> (failure)
                                     : result_t<void>::success ());
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
              else
                  admitted->complete (result_t<void>::success ());
              reply->complete (result);
          });
        end_at_admission_failure ();
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

    // Admission is already fixed by the source. This reply is also the
    // physical terminal, so it does not need an independent admission end.
    request_task_t (detail::request_projection_t,
                    task_t<TReply> reply,
                    std::shared_ptr<task_completion_source_t<void>> admitted) :
        task_t<TReply> (std::move (reply)),
        _physical_reply (detail::with_task_resume_scheduler (
          task_t<TReply> (this->_state),
          this->_resume_scheduler ? *this->_resume_scheduler : detail::task_scheduler_t{})),
        _admitted (std::move (admitted))
    {
        _physical_reply._release_turn_on_suspend = this->_release_turn_on_suspend;
    }

    /* The reply of a selected request in this reply type (hot path E4·E5:
     * one operation, one terminal). The selected request keeps its admission;
     * its physical terminal completes this reply through `project`, which owns
     * what the selected producer borrows until that terminal. This reply is
     * also the physical reply: no producer of its own outlives it. */
    template <typename TInput, typename TProject>
    request_task_t (detail::request_projection_t,
                    request_task_t<TInput> &selected,
                    TProject &&project) :
        task_t<TReply> (detail::project_task_terminal<TReply> (selected._physical_reply,
                                                               std::forward<TProject> (project))),
        _physical_reply (this->_state),
        _admitted (selected._admitted)
    {
    }

    /* The public reply of a request call over a selected request. Its physical
     * reply is the selected terminal in this reply type, so a nested owner can
     * still wait for the whole operation; the public reply also ends at an
     * admission failure (submit/completion §9). One observer completes both. */
    template <typename TInput, typename TProject>
    request_task_t (detail::public_request_projection_t,
                    request_task_t<TInput> &selected,
                    TProject &&project) :
        task_t<TReply> (std::make_shared<detail::application_task_shared_state_t<TReply>> ()),
        _physical_reply (detail::make_terminal_task_state<TReply> (
          *selected._physical_reply._state,
          [reply = this->_state, project = std::forward<TProject> (project)] (
            auto &physical, const result_t<TInput> &result) mutable {
              auto projected = [&] () -> result_t<TReply> {
                  try {
                      return project (result);
                  }
                  catch (...) {
                      return detail::current_exception_result<TReply> ();
                  }
              }();
              physical.complete (projected);
              reply->complete (std::move (projected));
          })),
        _admitted (selected._admitted)
    {
        end_at_admission_failure ();
    }

    // Submit/completion §9: an admission failure ends the public reply before
    // the physical terminal. A later terminal finds the reply already complete.
    void end_at_admission_failure ()
    {
        auto stage = _admitted->task ();
        detail::observe_task_terminal (stage, [reply = this->_state] (const auto &result) {
            if (!result)
                reply->complete (detail::result_access_t::failure<TReply> (result.exception ()));
        });
    }

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
    static request_task_t<T>
    from_admitted_reply (task_t<T> reply, std::shared_ptr<task_completion_source_t<void>> admitted)
    {
        return request_task_t<T> (request_projection_t{}, std::move (reply), std::move (admitted));
    }

    template <typename TOutput, typename TInput, typename TProject>
    static request_task_t<TOutput> project (request_task_t<TInput> &selected, TProject &&project)
    {
        return request_task_t<TOutput> (request_projection_t{}, selected,
                                        std::forward<TProject> (project));
    }

    template <typename TOutput, typename TInput, typename TProject>
    static request_task_t<TOutput> project_public (request_task_t<TInput> &selected,
                                                   TProject &&project)
    {
        return request_task_t<TOutput> (public_request_projection_t{}, selected,
                                        std::forward<TProject> (project));
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

// The selected child represents this same submitted call; infrastructure
// lookups are awaited directly instead of using this explicit connection.
template <typename T> request_submission_t<T> request_submission (request_task_t<T> pending)
{
    auto admitted = pending.admitted ();
    return {request_task_access_t::physical_reply (pending), std::move (admitted)};
}

/* Projects the selected request's physical terminal into another reply type
 * without a coroutine or a second admission source. Internal layers return
 * this projection; the public call adds its public reply once. */
template <typename TOutput, typename TInput, typename TProject>
request_task_t<TOutput> project_request_reply (request_task_t<TInput> &selected, TProject &&project)
{
    return request_task_access_t::project<TOutput> (selected, std::forward<TProject> (project));
}

template <typename TOutput, typename TInput, typename TProject>
request_task_t<TOutput> public_request_reply (request_task_t<TInput> &selected, TProject &&project)
{
    return request_task_access_t::project_public<TOutput> (selected,
                                                           std::forward<TProject> (project));
}

template <typename T>
request_task_t<T> reschedule_task (request_task_t<T> pending, task_scheduler_t scheduler)
{
    return request_task_access_t::reschedule (std::move (pending), std::move (scheduler));
}
}

} // namespace zlink::framework
