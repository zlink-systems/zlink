from pathlib import Path
base=Path('/root/worktree/zlink-3867r/framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch')
p=base/'task.hpp';s=Path('/root/jobs/fx/cx/v21/s033/crecv/task.hpp.before').read_text().replace('#include <memory>','#include <memory>\n#include <memory_resource>')
s=s.replace('    struct callback_t\n    {','    struct callback_t;\n    struct callback_deleter_t\n    {\n        void operator() (callback_t *callback) const noexcept { callback->destroy (); }\n    };\n    using callback_pointer_t = std::unique_ptr<callback_t, callback_deleter_t>;\n    struct callback_t\n    {',1)
s=s.replace('        virtual void invoke (const result_t<T> &) = 0;\n        std::unique_ptr<callback_t> next;','        virtual void invoke (const result_t<T> &) = 0;\n        virtual void destroy () noexcept = 0;\n        callback_pointer_t next;',1)
s=s.replace('terminal_callback_t (TCallback callback, ambient_context_snapshot_t snapshot) :','terminal_callback_t (TCallback callback, ambient_context_snapshot_t snapshot,\n                             std::pmr::memory_resource *resource) :',1)
s=s.replace('            owner (capture_current_serial_turn ())','            owner (capture_current_serial_turn ()),\n            resource (resource)',1)
s=s.replace('        TCallback callback;\n        ambient_context_snapshot_t ambient;','        void destroy () noexcept override\n        {\n            auto *selected = resource;\n            std::destroy_at (this);\n            selected->deallocate (this, sizeof (terminal_callback_t), alignof (terminal_callback_t));\n        }\n        TCallback callback;\n        std::pmr::memory_resource *resource;\n        ambient_context_snapshot_t ambient;',1)
# Correct init order
s=s.replace('            ambient (std::move (snapshot)),\n            owner (capture_current_serial_turn ()),\n            resource (resource)','            resource (resource),\n            ambient (std::move (snapshot)),\n            owner (capture_current_serial_turn ())',1)
s=s.replace('        std::unique_ptr<callback_t> terminal_callbacks;','        callback_pointer_t terminal_callbacks;',1)
s=s.replace('    virtual ~task_shared_state_t () = default;','    virtual ~task_shared_state_t () = default;\n\n    // Embedded request stages alias the request owner instead of acquiring\n    // independent shared ownership blocks. Ordinary tasks retain their owner.\n    virtual std::shared_ptr<task_shared_state_t> shared_from_this ()\n    {\n        return std::enable_shared_from_this<task_shared_state_t>::shared_from_this ();\n    }\n    virtual std::weak_ptr<task_shared_state_t> weak_from_this () noexcept\n    {\n        return std::enable_shared_from_this<task_shared_state_t>::weak_from_this ();\n    }',1)
s=s.replace('                auto terminal = std::make_unique<terminal_callback_t<TCallback>> (\n                  std::move (callback), std::move (snapshot));','                using terminal_t = terminal_callback_t<TCallback>;\n                auto *resource = terminal_resource ();\n                std::pmr::polymorphic_allocator<terminal_t> allocator (resource);\n                auto *storage = allocator.allocate (1);\n                try {\n                    std::construct_at (storage, std::move (callback), std::move (snapshot), resource);\n                }\n                catch (...) {\n                    allocator.deallocate (storage, 1);\n                    throw;\n                }\n                callback_pointer_t terminal (storage);',1)
s=s.replace('  protected:\n    virtual void bind_wait_owner','  protected:\n    virtual std::pmr::memory_resource *terminal_resource () noexcept\n    {\n        return std::pmr::new_delete_resource ();\n    }\n\n    virtual void bind_wait_owner',1)
s=s.replace('    std::unique_ptr<callback_t> _terminal_callbacks;','    callback_pointer_t _terminal_callbacks;',1)
s=s.replace('    template <typename U>\n    friend std::shared_ptr<task_completion_source_t<U>> detail::make_task_completion_source ();','    template <typename U>\n    friend std::shared_ptr<task_completion_source_t<U>> detail::make_task_completion_source ();\n    friend struct detail::request_task_access_t;',1) if False else s
# forward declaration friend
s=s.replace('template <typename T> class task_shared_state_t;','struct request_task_access_t;\ntemplate <typename T> class task_shared_state_t;',1)
s=s.replace('    // The shared-source allocation owns this wrapper and state.', '    friend struct detail::request_task_access_t;\n\n    // The shared-source allocation owns this wrapper and state.',1)
p.write_text(s)
p=base/'request_task.hpp';s=Path('/root/jobs/fx/cx/v21/s033/crecv/request_task.hpp.before').read_text().replace('#include <zlink/framework/contracts/dispatch/task.hpp>','#include <zlink/framework/contracts/dispatch/task.hpp>\n\n#include <array>\n#include <cstddef>')
pos=s.index('// Copies retain')
s=s[:pos]+'''// Capture storage belongs to one request, including codec captures added by
// internal layers. It is not a registry or an execution queue. Oversize custom
// captures use the resource's upstream allocator without imposing a request limit.
class request_capture_storage_t
{
    struct capture_t
    {
        capture_t *next;
        void (*destroy) (capture_t *) noexcept;
    };
    template <typename T> struct stored_capture_t : capture_t
    {
        explicit stored_capture_t (T value) :
            capture_t{nullptr, [] (capture_t *capture) noexcept {
                          std::destroy_at (static_cast<stored_capture_t *> (capture));
                      }},
            value (std::move (value))
        {
        }
        T value;
    };

  public:
    request_capture_storage_t () = default;
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
    template <typename T> T *store (T value)
    {
        using stored_t = stored_capture_t<T>;
        auto *storage = _resource.allocate (sizeof (stored_t), alignof (stored_t));
        auto *capture = std::construct_at (static_cast<stored_t *> (storage), std::move (value));
        capture->next = _captures;
        _captures = capture;
        return &capture->value;
    }
    std::pmr::memory_resource *resource () noexcept { return &_resource; }

  private:
    alignas (std::max_align_t) std::array<std::byte, 1024> _buffer;
    std::pmr::monotonic_buffer_resource _resource{_buffer.data (), _buffer.size ()};
    capture_t *_captures = nullptr;
};

''' + s[pos:]
s=s.replace('    explicit operator bool () const noexcept { return bool (_owner); }','''    // Stored downstream callbacks borrow this block. The outward callback owns
    // it, so composing a projection cannot create a request ownership cycle.
    request_callback_t borrow () const
    {
        auto selected = *this;
        selected._owner = std::shared_ptr<void> (std::shared_ptr<void>{}, _owner.get ());
        return selected;
    }

    template <typename... TNextArgs, typename TCallback>
    request_callback_t<TNextArgs...> capture (TCallback callback) const
    {
        if (!_storage)
            return request_callback_t<TNextArgs...> (std::move (callback));
        auto *stored = _storage->store (std::move (callback));
        return request_callback_t<TNextArgs...>::from_storage (_owner, _storage, stored);
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

    explicit operator bool () const noexcept { return bool (_owner); }''',1)
s=s.replace('    std::shared_ptr<void> _owner;\n    void (*_invoke)', '    request_capture_storage_t *_storage = nullptr;\n    std::shared_ptr<void> _owner;\n    void (*_invoke)',1)
s=s.replace('        auto storage = std::make_shared<storage_t> (\n          storage_t{std::move (complete), std::forward<TProject> (project)});\n        return {std::move (admitted),\n                request_callback_t<const result_t<TInput> &>::from_shared (std::move (storage)),\n                std::move (fail_public)};', '''        auto projected = complete.template capture<const result_t<TInput> &> (
          storage_t{complete.borrow (), std::forward<TProject> (project)});
        return {std::move (admitted), std::move (projected), std::move (fail_public)};''',1)
# borrow entire receiver for captures
s=s.replace('    void observe (request_task_t<T> selected);','''    request_receiver_t borrow () const
    {
        return {std::shared_ptr<task_completion_source_t<void>> (
                  std::shared_ptr<task_completion_source_t<void>>{}, admitted.get ()),
                complete.borrow (), fail_public.borrow ()};
    }

    void observe (request_task_t<T> selected);''',1)
# replace receive_public receive_internal entry assembly
start=s.index('        return receive<TOutput> (\n',s.index('static request_task_t<TOutput> receive_public'))
end=s.index('\n    template <typename T>\n    static request_task_t<T> reschedule',start)
s=s[:start]+'''        return receive<TOutput, TInput, true> (std::forward<TSubmit> (submit),
                                               std::forward<TProject> (project));
    }

    template <typename T, typename TSubmit>
    static request_task_t<T> receive_internal (TSubmit &&submit)
    {
        return receive<T, T, false> (std::forward<TSubmit> (submit),
                                     [] (const result_t<T> &result) { return result; });
    }
''' + s[end:]
start=s.index('    template <typename T, typename TTag, typename TSubmit, typename TReceiver>')
end=s.index('\n};\n\ntemplate <typename T> void request_receiver_t',start)
s=s[:start]+'''    template <typename T, typename TInput, typename TProject, bool Public>
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
            std::pmr::memory_resource *terminal_resource () noexcept override
            {
                return owner.resource ();
            }
            receiver_state_t &owner;
        };

        explicit receiver_state_t (TProject project) :
            admitted (this), reply (*this), physical (*this), project (std::move (project))
        {
        }
        std::pmr::memory_resource *terminal_resource () noexcept override { return resource (); }
        void operator() (const result_t<TInput> &input)
        {
            auto projected = [&] () -> result_t<T> {
                try {
                    return project (input);
                }
                catch (...) {
                    return current_exception_result<T> ();
                }
            }();
            if constexpr (Public)
                physical.complete (projected);
            reply.complete (std::move (projected));
        }
        task_completion_source_t<void> admitted;
        stage_t<T> reply;
        stage_t<T> physical;
        TProject project;
    };

    template <typename T, typename TInput, bool Public, typename TSubmit, typename TProject>
    static request_task_t<T> receive (TSubmit &&submit, TProject &&project)
    {
        using storage_t = receiver_state_t<T, TInput, std::decay_t<TProject>, Public>;
        auto storage = std::make_shared<storage_t> (std::forward<TProject> (project));
        auto admitted = std::shared_ptr<task_completion_source_t<void>> (storage, &storage->admitted);
        auto reply = std::shared_ptr<task_shared_state_t<T>> (storage, &storage->reply);
        request_task_t<T> pending (request_projection_t{}, task_t<T> (reply), admitted);
        if constexpr (Public) {
            pending._physical_reply = task_t<T> (
              std::shared_ptr<task_shared_state_t<T>> (storage, &storage->physical));
            // The admission observer is embedded in this block and borrows the
            // public stage. A failed admission must not release physical captures.
            storage->on_terminal ([public_reply = &storage->reply] (const result_t<void> &result) {
                if (!result)
                    public_reply->complete (result_access_t::failure<T> (result.exception ()));
            });
        }
        request_receiver_t<TInput> receiver{
          admitted,
          request_callback_t<const result_t<TInput> &>::from_storage (storage, storage.get (),
                                                                      storage.get ()),
          request_callback_t<std::exception_ptr> (reply, [] (auto &reply, std::exception_ptr failure) {
              reply.complete (result_access_t::failure<T> (std::move (failure)));
          })};
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
    }''' + s[end:]
# task_t private ctor request_access isn't friend. add friend in task.hpp next
p.write_text(s)
p=base/'task.hpp';s=p.read_text()
s=s.replace('    friend class task_completion_source_t<void>;','    friend struct detail::request_task_access_t;\n    friend class task_completion_source_t<void>;',1)
# Move deleter implementation after complete state class
s=s.replace('        void operator() (callback_t *callback) const noexcept { callback->destroy (); }','        void operator() (callback_t *callback) const noexcept;',1)
idx=s.index('template <typename T> class application_task_shared_state_t')
s=s[:idx]+'''template <typename T>
void task_shared_state_t<T>::callback_deleter_t::operator() (callback_t *callback) const noexcept
{
    callback->destroy ();
}

'''+s[idx:]
p.write_text(s)
