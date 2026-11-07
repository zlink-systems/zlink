/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <array>
#include <cstddef>
#include <memory_resource>

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

// Capture storage belongs to one request, including codec captures added by
// internal layers. It is not a registry or an execution queue. Oversize custom
// captures use the resource's upstream allocator without imposing a request limit.
class request_capture_storage_t
{
    struct capture_t
    {
        capture_t *next;
        void (*destroy) (capture_t *) noexcept;
        void (*release) (capture_t *) noexcept;
    };
    template <typename T, bool Callable> struct stored_capture_t : capture_t
    {
        explicit stored_capture_t (T value) :
            capture_t{nullptr,
                      [] (capture_t *capture) noexcept {
                          std::destroy_at (static_cast<stored_capture_t *> (capture));
                      },
                      [] (capture_t *capture) noexcept {
                          if constexpr (Callable)
                              static_cast<stored_capture_t *> (capture)->value.reset ();
                      }},
            value (std::move (value))
        {
        }
        template <typename... TArgs> void operator() (TArgs &&...arguments)
        {
            // Keep the capture's address stable through physical processing;
            // retained public task handles must not retain completed captures.
            const auto reset = [] (std::optional<T> *capture) { capture->reset (); };
            std::unique_ptr<std::optional<T>, decltype (reset)> lifetime (&value, reset);
            (*value) (std::forward<TArgs> (arguments)...);
        }
        std::optional<T> value;
    };

  public:
    explicit request_capture_storage_t (
      void (*release_project) (request_capture_storage_t *) noexcept) :
        _release_project (release_project)
    {
    }

    // The allocator keeps the request block alive through control-block
    // destruction and deallocation. Its memory is already in this request.
    template <typename T> struct allocator_t
    {
        using value_type = T;
        explicit allocator_t (std::shared_ptr<request_capture_storage_t> storage) :
            storage (std::move (storage))
        {
        }
        template <typename U> allocator_t (const allocator_t<U> &other) : storage (other.storage) {}
        T *allocate (std::size_t count)
        {
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
            test_creation_probe::request_callback_owner_bytes.fetch_add (count * sizeof (T),
                                                                         std::memory_order_relaxed);
#endif
            return static_cast<T *> (storage->_resource.allocate (count * sizeof (T), alignof (T)));
        }
        void deallocate (T *, std::size_t) noexcept {}
        template <typename U> bool operator== (const allocator_t<U> &other) const noexcept
        {
            return storage == other.storage;
        }
        std::shared_ptr<request_capture_storage_t> storage;
    };
    struct callback_lifetime_t
    {
        explicit callback_lifetime_t (request_capture_storage_t &storage) : storage (storage) {}
        ~callback_lifetime_t ()
        {
            // Task handles retain results, not callbacks that will never run.
            for (auto *capture = storage._captures; capture; capture = capture->next)
                capture->release (capture);
            storage._release_project (&storage);
        }
        request_capture_storage_t &storage;
    };
    template <typename TOwner>
    static std::shared_ptr<TOwner> own_callbacks (const std::shared_ptr<TOwner> &storage)
    {
        auto lifetime = std::allocate_shared<callback_lifetime_t> (
          allocator_t<callback_lifetime_t> (storage), *storage);
        return {std::move (lifetime), storage.get ()};
    }
    request_capture_storage_t (const request_capture_storage_t &) = delete;
    request_capture_storage_t &operator= (const request_capture_storage_t &) = delete;
    ~request_capture_storage_t ()
    {
        while (_captures) {
            auto *capture = _captures;
            _captures = capture->next;
            capture->destroy (capture);
        }
    }
    std::pmr::memory_resource *resource () noexcept { return &_resource; }
    // One C1 completion travels from registration to its independent delivery turn.
    // It has explicit destruction ownership and does not consume capture arena space.
    static constexpr std::size_t completion_storage_size = 512;
    void *completion_storage () noexcept { return _completion.data (); }

    template <bool Callable = true, typename T> auto *store (T value)
    {
        using stored_t = stored_capture_t<T, Callable>;
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
        test_creation_probe::request_capture_bytes.fetch_add (sizeof (stored_t),
                                                              std::memory_order_relaxed);
        test_creation_probe::request_capture_value_bytes.fetch_add (sizeof (T),
                                                                    std::memory_order_relaxed);
#endif
        auto *storage = _resource.allocate (sizeof (stored_t), alignof (stored_t));
        auto *capture = std::construct_at (static_cast<stored_t *> (storage), std::move (value));
        capture->next = _captures;
        _captures = capture;
        return capture;
    }

  private:
    alignas (std::max_align_t) std::array<std::byte, completion_storage_size> _completion;
    alignas (std::max_align_t) std::array<std::byte, 640> _buffer;
    std::pmr::monotonic_buffer_resource _resource{_buffer.data (), _buffer.size ()};
    capture_t *_captures = nullptr;
    void (*_release_project) (request_capture_storage_t *) noexcept;
};

// Copies retain the same operation's callable without cloning its captures.
template <typename... TArgs> class request_callback_t
{
  public:
    request_callback_t () = default;
    request_callback_t (std::nullptr_t) {}
    template <typename TCallback>
        requires (!std::is_same_v<std::decay_t<TCallback>, request_callback_t>)
    request_callback_t (TCallback &&callback)
    {
        if constexpr (requires { static_cast<bool> (callback); }) {
            if (!callback)
                return;
        }
        *this = from_shared (
          std::make_shared<std::decay_t<TCallback>> (std::forward<TCallback> (callback)));
    }

    template <typename TOwner, typename TInvoke>
        requires (std::is_empty_v<TInvoke> && std::is_default_constructible_v<TInvoke>)
    request_callback_t (std::shared_ptr<TOwner> owner,
                        TInvoke,
                        request_capture_storage_t *storage = nullptr) :
        _storage (storage),
        _owner (std::move (owner)),
        _invoke ([] (void *owner, TArgs... arguments) {
            TInvoke{}(*static_cast<TOwner *> (owner), std::forward<TArgs> (arguments)...);
        })
    {
    }

    template <typename TCallback>
    static request_callback_t from_shared (std::shared_ptr<TCallback> callback)
    {
        request_callback_t selected;
        selected._owner = std::move (callback);
        selected._invoke = [] (void *owner, TArgs... arguments) {
            (*static_cast<TCallback *> (owner)) (std::forward<TArgs> (arguments)...);
        };
        return selected;
    }

    // Stored downstream callbacks borrow this block. The outward callback owns
    // it, so composing a projection cannot create a request ownership cycle.
    request_callback_t borrow () const
    {
        if (!_storage)
            return *this;
        request_callback_t selected;
        selected._storage = _storage;
        selected._invoke = _invoke;
        selected._owner = std::shared_ptr<void> (std::shared_ptr<void>{}, _owner.get ());
        return selected;
    }

    template <typename... TNextArgs, typename TCallback>
    request_callback_t<TNextArgs...> capture (TCallback callback) const
    {
        if (!_storage) {
            auto stored = std::make_shared<TCallback> (std::move (callback));
            return request_callback_t<TNextArgs...>::from_shared (std::move (stored));
        }
        auto *stored = _storage->store (std::move (callback));
        return request_callback_t<TNextArgs...>::from_storage (_owner, _storage, stored);
    }

    // Values borrowed by submit and by physical conversion share this request
    // block. The caller retains the alias until submit returns; projections
    // stored in the block borrow it to avoid a cycle.
    template <typename TValue> std::shared_ptr<TValue> retain (TValue value) const
    {
        if (!_storage)
            return std::make_shared<TValue> (std::move (value));
        auto *stored = _storage->store<false> (std::move (value));
        return std::shared_ptr<TValue> (_owner, std::addressof (*stored->value));
    }

    bool has_request_storage () const noexcept { return _storage != nullptr; }
    std::shared_ptr<void> completion_storage () const noexcept
    {
        return _storage ? std::shared_ptr<void> (_owner, _storage->completion_storage ())
                        : std::shared_ptr<void>{};
    }
    std::pmr::memory_resource *resource () const noexcept
    {
        return _storage ? _storage->resource () : std::pmr::get_default_resource ();
    }

    template <typename TOwner, typename TCallback>
    static request_callback_t from_storage (const std::shared_ptr<TOwner> &owner,
                                            request_capture_storage_t *storage,
                                            TCallback *callback)
    {
        auto selected = from_shared (std::shared_ptr<TCallback> (owner, callback));
        selected._storage = storage;
        return selected;
    }

    explicit operator bool () const noexcept { return bool (_owner); }
    void operator() (TArgs... arguments) const
    {
        if (!_owner)
            throw std::bad_function_call ();
        const auto invoke = _invoke;
        const auto owner = _owner;
        invoke (owner.get (), std::forward<TArgs> (arguments)...);
    }

  private:
    template <typename...> friend class request_callback_t;
    request_capture_storage_t *_storage = nullptr;
    std::shared_ptr<void> _owner;
    void (*_invoke) (void *, TArgs...) = nullptr;
};

// The selected operation delivers into the final reply. Codec owners compose
// synchronous conversions without adding asynchronous result states.
// Completion and public failure retain independent callable ownership.
template <typename T> struct request_receiver_t
{
    std::shared_ptr<task_completion_source_t<void>> admitted;
    request_callback_t<const result_t<T> &> complete;
    request_callback_t<std::exception_ptr> fail_public;

    template <typename TInput, typename TProject> struct projection_t
    {
        request_callback_t<const result_t<T> &> deliver;
        TProject project;
        void operator() (const result_t<TInput> &terminal)
        {
            auto projected = [&] () -> result_t<T> {
                try {
                    return project (terminal);
                }
                catch (...) {
                    return current_exception_result<T> ();
                }
            }();
            deliver (projected);
        }
    };

    template <typename TInput, typename TProject>
    request_receiver_t<TInput> project (TProject &&project)
    {
        using storage_t = projection_t<TInput, std::decay_t<TProject>>;
        auto projected = complete.template capture<const result_t<TInput> &> (
          storage_t{complete.borrow (), std::forward<TProject> (project)});
        return {std::move (admitted), std::move (projected), std::move (fail_public)};
    }

    request_receiver_t borrow () const
    {
        return {complete.has_request_storage ()
                  ? std::shared_ptr<task_completion_source_t<void>> (
                      std::shared_ptr<task_completion_source_t<void>>{}, admitted.get ())
                  : admitted,
                complete.borrow (), fail_public.borrow ()};
    }

    void observe (request_task_t<T> selected);
};

// This signal connects only the selected submission of the same operation.
// Ordinary nested request awaits do not publish admission to their caller.
template <typename T> struct request_submission_t
{
    task_t<T> reply;
    task_t<void> admitted;
    task_t<T> public_reply;
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
            auto pending =
              request_task_t (task_t<TReply>::promise_type::get_return_object (), _admitted);
            _public_reply = pending.shared_state ();
            return pending;
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
            detail::observe_task_terminal (
              submission.public_reply,
              [reply = _public_reply,
               physical = submission.reply.shared_state ()] (const auto &result) {
                  if (auto failure = detail::request_failure (result)) {
                      // A physical failure still belongs to the caller's codec/error mapping.
                      // Forward only a public terminal that preceded that physical result.
                      if (!physical->is_ready ()
                          || detail::request_failure (physical->result ()) != failure)
                          reply->complete (
                            detail::result_access_t::failure<TReply> (std::move (failure)));
                  }
              });
            return std::move (submission.reply);
        }

      private:
        std::shared_ptr<detail::task_shared_state_t<TReply>> _public_reply;
        std::shared_ptr<task_completion_source_t<void>> _admitted =
          std::make_shared<task_completion_source_t<void>> ();
    };

    request_task_t (result_t<TReply> result) :
        task_t<TReply> (result),
        _physical_reply (this->shared_state ()),
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
        (void) _physical_reply.shared_state ();
        detail::observe_task_terminal (
          _physical_reply,
          [reply = this->shared_state (), admitted = _admitted] (const auto &result) {
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
          task_t<TReply> (this->shared_state ()),
          this->_resume_scheduler ? *this->_resume_scheduler : detail::task_scheduler_t{})),
        _admitted (std::move (admitted))
    {
        _physical_reply._release_turn_on_suspend = this->_release_turn_on_suspend;
    }

    template <typename TInput, typename TProject>
    detail::request_receiver_t<TInput> receiver (detail::public_request_projection_t,
                                                 TProject &&project)
    {
        return {
          _admitted,
          [reply = this->shared_state (), physical = _physical_reply.shared_state (),
           project = std::forward<TProject> (project)] (const result_t<TInput> &input) mutable {
              auto projected = [&] () -> result_t<TReply> {
                  try {
                      return project (input);
                  }
                  catch (...) {
                      return detail::current_exception_result<TReply> ();
                  }
              }();
              physical->complete (projected);
              reply->complete (std::move (projected));
          },
          detail::request_callback_t<std::exception_ptr> (
            this->shared_state (), [] (auto &reply, std::exception_ptr failure) {
                reply.complete (detail::result_access_t::failure<TReply> (std::move (failure)));
            })};
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
        task_t<TReply> (std::make_shared<detail::application_task_shared_state_t<TReply>> ()),
        _physical_reply (this->shared_state ()),
        _admitted (selected._admitted)
    {
        detail::observe_task_terminal (
          selected._physical_reply,
          [reply = this->shared_state (),
           project = std::forward<TProject> (project)] (const auto &result) mutable {
              try {
                  reply->complete (project (result));
              }
              catch (...) {
                  reply->complete (detail::current_exception_result<TReply> ());
              }
          });
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
        _physical_reply (std::make_shared<detail::application_task_shared_state_t<TReply>> ()),
        _admitted (selected._admitted)
    {
        auto terminal = receiver<TInput> (detail::public_request_projection_t{},
                                          std::forward<TProject> (project));
        detail::observe_task_terminal (selected._physical_reply, std::move (terminal.complete));
        end_at_admission_failure ();
    }

    // Submit/completion §9: an admission failure ends the public reply before
    // the physical terminal. A later terminal finds the reply already complete.
    void end_at_admission_failure ()
    {
        auto stage = _admitted->task ();
        detail::observe_task_terminal (stage, [reply = this->shared_state ()] (const auto &result) {
            if (!result)
                reply->complete (detail::result_access_t::failure<TReply> (result.exception ()));
        });
    }

    task_t<TReply> public_reply () { return task_t<TReply> (this->shared_state ()); }

    task_t<TReply> physical_reply () const
    {
        return detail::with_task_resume_scheduler (task_t<TReply> (_physical_reply.shared_state ()),
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

    template <typename T> static auto public_reply (request_task_t<T> &task)
    {
        return task.public_reply ();
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

    template <typename T, typename TSubmit>
    static request_task_t<T> receive_public (TSubmit &&submit)
    {
        return receive_public<T, T> (std::forward<TSubmit> (submit),
                                     [] (const result_t<T> &result) { return result; });
    }

    template <typename TOutput, typename TInput, typename TSubmit, typename TProject>
    static request_task_t<TOutput> receive_public (TSubmit &&submit, TProject &&project)
    {
        return receive<TOutput, TInput, true> (std::forward<TSubmit> (submit),
                                               std::forward<TProject> (project));
    }

    template <typename T, typename TSubmit>
    static request_task_t<T> receive_internal (TSubmit &&submit)
    {
        return receive<T, T, false> (std::forward<TSubmit> (submit),
                                     [] (const result_t<T> &result) { return result; });
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

  private:
    template <typename T, typename TInput, typename TProject, bool Public>
    struct receiver_state_t final : request_capture_storage_t, application_task_shared_state_t<void>
    {
        template <typename TValue> struct stage_t final : application_task_shared_state_t<TValue>
        {
            explicit stage_t (receiver_state_t &owner) : owner (owner) {}
            std::shared_ptr<task_shared_state_t<TValue>> shared_from_this () override
            {
                return {owner.application_task_shared_state_t<void>::shared_from_this (), this};
            }
            std::weak_ptr<task_shared_state_t<TValue>> weak_from_this () noexcept override
            {
                return shared_from_this ();
            }
            receiver_state_t &owner;
        };

        explicit receiver_state_t (TProject project) :
            request_capture_storage_t ([] (request_capture_storage_t *storage) noexcept {
                static_cast<receiver_state_t *> (storage)->project.reset ();
            }),
            admitted (this),
            reply (*this),
            physical (make_physical (*this)),
            project (std::move (project))
        {
        }
        void publish_terminal (const result_t<void> &result) override
        {
            if constexpr (Public) {
                if (!result)
                    reply.complete (result_access_t::failure<T> (result.exception ()));
            }
        }
        void operator() (const result_t<TInput> &input)
        {
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
            test_creation_probe::request_deliveries.fetch_add (1, std::memory_order_relaxed);
#endif
            auto projected = [&] () -> result_t<T> {
                try {
                    return (*project) (input);
                }
                catch (...) {
                    return current_exception_result<T> ();
                }
            }();
            project.reset ();
            if constexpr (Public)
                physical.complete (projected);
            reply.complete (std::move (projected));
        }
        task_completion_source_t<void> admitted;
        stage_t<T> reply;
        static auto make_physical (receiver_state_t &owner)
        {
            if constexpr (Public)
                return stage_t<T> (owner);
            else
                return std::monostate{};
        }
        std::conditional_t<Public, stage_t<T>, std::monostate> physical;
        std::optional<TProject> project;
    };

    template <typename T, typename TInput, bool Public, typename TSubmit, typename TProject>
    static request_task_t<T> receive (TSubmit &&submit, TProject &&project)
    {
        using storage_t = receiver_state_t<T, TInput, std::decay_t<TProject>, Public>;
        auto storage = std::make_shared<storage_t> (std::forward<TProject> (project));
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
        test_creation_probe::request_block_bytes.fetch_add (sizeof (storage_t),
                                                            std::memory_order_relaxed);
#endif
        auto admitted =
          std::shared_ptr<task_completion_source_t<void>> (storage, &storage->admitted);
        auto reply = std::shared_ptr<task_shared_state_t<T>> (storage, &storage->reply);
        request_task_t<T> pending (request_projection_t{}, task_t<T> (reply), admitted);
        if constexpr (Public) {
            pending._physical_reply =
              task_t<T> (std::shared_ptr<task_shared_state_t<T>> (storage, &storage->physical));
        }
        auto callbacks = request_capture_storage_t::own_callbacks (storage);
        request_receiver_t<TInput> receiver{
          admitted,
          request_callback_t<const result_t<TInput> &>::from_storage (callbacks, storage.get (),
                                                                      storage.get ()),
          request_callback_t<std::exception_ptr> (
            reply,
            [] (auto &reply, std::exception_ptr failure) {
                reply.complete (result_access_t::failure<T> (std::move (failure)));
            },
            storage.get ())};
        try {
            std::forward<TSubmit> (submit) (std::move (receiver));
        }
        catch (...) {
            const auto failure = current_exception_result<T> ();
            admitted->complete (result_access_t::failure<void> (failure.exception ()));
            if constexpr (Public)
                storage->physical.complete (failure);
            storage->reply.complete (failure);
        }
        return pending;
    }
};

template <typename T> void request_receiver_t<T>::observe (request_task_t<T> selected)
{
    if (request_task_access_t::admission_source (selected) != admitted) {
        auto acceptance = selected.admitted ();
        observe_task_terminal (acceptance, [source = admitted] (const result_t<void> &result) {
            source->complete (result);
        });
    }
    auto physical = request_task_access_t::physical_reply (selected);
    observe_task_terminal (physical, std::move (complete));
}

// The selected child represents this same submitted call; infrastructure
// lookups are awaited directly instead of using this explicit connection.
template <typename T> request_submission_t<T> request_submission (request_task_t<T> pending)
{
    auto admitted = pending.admitted ();
    return {request_task_access_t::physical_reply (pending), std::move (admitted),
            request_task_access_t::public_reply (pending)};
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
