from pathlib import Path
p=Path('/root/worktree/zlink-3867f/framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp')
s=p.read_text().replace('        void (*destroy) (capture_t *) noexcept;', '        void (*destroy) (capture_t *) noexcept;\n        void (*release) (capture_t *) noexcept;').replace('template <typename T> struct stored_capture_t : capture_t', 'template <typename T, bool Callable> struct stored_capture_t : capture_t').replace('''                          std::destroy_at (static_cast<stored_capture_t *> (capture));
                      }},''','''                          std::destroy_at (static_cast<stored_capture_t *> (capture));
                      },
                      [] (capture_t *capture) noexcept {
                          if constexpr (Callable)
                              static_cast<stored_capture_t *> (capture)->value.reset ();
                      }},''')
s=s.replace('    request_capture_storage_t () = default;', '''    explicit request_capture_storage_t (void (*release_project) (request_capture_storage_t *) noexcept) :
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
    template <typename TOwner> static std::shared_ptr<TOwner> own_callbacks (
      const std::shared_ptr<TOwner> &storage)
    {
        auto lifetime = std::allocate_shared<callback_lifetime_t> (
          allocator_t<callback_lifetime_t> (storage), *storage);
        return {std::move (lifetime), storage.get ()};
    }''')
s=s.replace('template <typename T> auto *store (T value)', 'template <bool Callable = true, typename T> auto *store (T value)').replace('using stored_t = stored_capture_t<T>;', 'using stored_t = stored_capture_t<T, Callable>;')
s=s.replace('    capture_t *_captures = nullptr;', '''    capture_t *_captures = nullptr;
    void (*_release_project) (request_capture_storage_t *) noexcept;''')
s=s.replace('auto *stored = _storage->store (std::move (value));','auto *stored = _storage->store<false> (std::move (value));')
s=s.replace('''        explicit receiver_state_t (TProject project) :
            admitted (this),''','''        explicit receiver_state_t (TProject project) :
            request_capture_storage_t ([] (request_capture_storage_t *storage) noexcept {
                static_cast<receiver_state_t *> (storage)->project.reset ();
            }),
            admitted (this),''')
s=s.replace('''        request_receiver_t<TInput> receiver{
          admitted,''','''        auto callbacks = request_capture_storage_t::own_callbacks (storage);
        request_receiver_t<TInput> receiver{
          admitted,''').replace('''request_callback_t<const result_t<TInput> &>::from_storage (storage, storage.get (),
                                                                      storage.get ())''','''request_callback_t<const result_t<TInput> &>::from_storage (callbacks, storage.get (),
                                                                      storage.get ())''')
p.write_text(s)
