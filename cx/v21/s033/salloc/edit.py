from pathlib import Path
root=Path('/root/worktree/zlink-3867a/framework/languages/cpp/framework')
p=root/'include/zlink/framework/contracts/dispatch/request_task.hpp'
s=p.read_text();a=s.index('    std::function<void (const result_t<T> &)> complete;');b=s.index('\n    void observe (request_task_t<T> selected);',a)
s=s[:a]+'''    template <typename TComplete, typename TFail = std::nullptr_t>
    request_receiver_t (std::shared_ptr<task_completion_source_t<void>> admission,
                        TComplete complete,
                        TFail fail = nullptr) :
        admitted (std::move (admission)),
        _terminal (std::make_shared<terminal_t<TComplete, TFail>> (std::move (complete),
                                                                  std::move (fail)))
    {
    }

    void complete (const result_t<T> &result) const { _terminal->complete (result); }
    void fail_public (std::exception_ptr failure) const
    {
        _terminal->fail_public (std::move (failure));
    }

    template <typename TInput, typename TProject>
    request_receiver_t<TInput> project (TProject &&project)
    {
        auto failure = _terminal;
        return {std::move (admitted),
                [deliver = std::move (_terminal), project = std::forward<TProject> (project)] (
                  const result_t<TInput> &terminal) mutable {
                    auto projected = [&] () -> result_t<T> {
                        try {
                            return project (terminal);
                        }
                        catch (...) {
                            return current_exception_result<T> ();
                        }
                    }();
                    deliver->complete (projected);
                },
                [failure = std::move (failure)] (std::exception_ptr error) {
                    failure->fail_public (std::move (error));
                }};
    }
''' + s[b:]
needle='    void observe (request_task_t<T> selected);'
s=s.replace(needle,needle+'''

  private:
    struct terminal_base_t
    {
        virtual ~terminal_base_t () = default;
        virtual void complete (const result_t<T> &) = 0;
        virtual void fail_public (std::exception_ptr) = 0;
    };
    template <typename TComplete, typename TFail> struct terminal_t final : terminal_base_t
    {
        terminal_t (TComplete complete, TFail fail) :
            deliver (std::move (complete)), fail (std::move (fail))
        {
        }
        void complete (const result_t<T> &result) override { deliver (result); }
        void fail_public (std::exception_ptr error) override
        {
            if constexpr (std::is_same_v<TFail, std::nullptr_t>)
                throw std::bad_function_call ();
            else
                fail (std::move (error));
        }
        TComplete deliver;
        TFail fail;
    };
    std::shared_ptr<terminal_base_t> _terminal;
''')
s=s.replace('detail::observe_task_terminal (selected._physical_reply, std::move (terminal.complete));','detail::observe_task_terminal (\n          selected._physical_reply, [terminal = std::move (terminal)] (const auto &result) {\n              terminal.complete (result);\n          });')
s=s.replace('observe_task_terminal (physical, std::move (complete));','observe_task_terminal (physical, [receiver = std::move (*this)] (const auto &result) {\n        receiver.complete (result);\n    });')
p.write_text(s)
p=root/'src/runtime/channels/route_channel_runtime.cpp';s=p.read_text().replace('deliver = std::move (receiver.complete)', 'receiver = std::move (receiver)').replace('                        deliver (terminal);','                        receiver.complete (terminal);');p.write_text(s)
p=root/'src/runtime/mesh/mesh_node_runtime.cpp';s=p.read_text().replace('[fail = receiver.fail_public] (const auto &result)', '[receiver] (const auto &result)').replace('            fail (std::move (failure));','            receiver.fail_public (std::move (failure));');p.write_text(s)
p=root/'include/zlink/framework/contracts/dispatch/task.hpp';s=p.read_text();a=s.index('    struct callback_t\n',s.index('class task_shared_state_t :'));b=s.index('\n  public:',a)
s=s[:a]+'''    struct callback_t
    {
        callback_t () : ambient (capture_ambient_context ()), owner (capture_current_serial_turn ())
        {
        }
        virtual ~callback_t () = default;
        virtual void invoke (const result_t<T> &) = 0;
        ambient_context_snapshot_t ambient;
        std::shared_ptr<serial_turn_t> owner;
        std::unique_ptr<callback_t> next;
    };
    template <typename TCallback> struct terminal_callback_t final : callback_t
    {
        explicit terminal_callback_t (TCallback callback) : callback (std::move (callback)) {}
        void invoke (const result_t<T> &result) override { callback (result); }
        TCallback callback;
    };
''' + s[b:]
s=s.replace('std::vector<callback_t> terminal_callbacks;', 'std::unique_ptr<callback_t> terminal_callbacks;')
s=s.replace('terminal_callbacks = std::move (_terminal_callbacks);','terminal_callbacks = std::move (_terminal_callbacks);\n            _terminal_callbacks_tail = nullptr;')
s=s.replace('''            for (auto &callback : terminal_callbacks) {
                const auto ambient_guard = enter_ambient_context (callback.ambient);
                callback.invoke (*_result);
            }''','''            while (terminal_callbacks) {
                auto callback = std::move (terminal_callbacks);
                terminal_callbacks = std::move (callback->next);
                const auto ambient_guard = enter_ambient_context (callback->ambient);
                callback->invoke (*_result);
            }''')
s=s.replace('''    void on_completed (std::function<void (const result_t<T> &)> callback,
                       task_scheduler_t scheduler = {})''','''    template <typename TCallback>
    void on_completed (TCallback callback, task_scheduler_t scheduler = {})''')
a=s.index('                registration = std::make_shared<task_wait_registration_t> (',s.index('void on_completed'));b=s.index('                append_continuation_unlocked (registration);',a)
s=s[:a]+'''                struct observer_t final : task_wait_registration_t
                {
                    observer_t (std::weak_ptr<task_shared_state_t<T>> owner,
                                TCallback callback,
                                task_scheduler_t scheduler) :
                        task_wait_registration_t (
                          [this] (bool cancelled, const ambient_context_snapshot_t &ambient) {
                              const auto ambient_guard = enter_ambient_context (ambient);
                              if (cancelled) {
                                  const auto failure = result_t<T>::failure (
                                    framework_error_kind_t::shutting_down,
                                    registered_execution_context_shutting_down_message);
                                  this->callback (failure);
                              } else if (auto self = this->owner.lock ())
                                  this->callback (*self->_result);
                          },
                          std::move (scheduler)),
                        owner (std::move (owner)), callback (std::move (callback))
                    {
                    }
                    std::weak_ptr<task_shared_state_t<T>> owner;
                    TCallback callback;
                };
                registration = std::make_shared<observer_t> (
                  owner_state, std::move (callback), std::move (scheduler));
''' + s[b:]
a=s.index('    void on_terminal (std::function<void (const result_t<T> &)> callback)');b=s.index('\n  private:',a)
s=s[:a]+'''    template <typename TCallback> void on_terminal (TCallback callback)
    {
        auto snapshot = capture_ambient_context ();
        {
            std::lock_guard lock (_mutex);
            if (!_result) {
                auto terminal = std::make_unique<terminal_callback_t<TCallback>> (
                  std::move (callback));
                terminal->ambient = std::move (snapshot);
                auto *tail = terminal.get ();
                if (_terminal_callbacks_tail)
                    _terminal_callbacks_tail->next = std::move (terminal);
                else
                    _terminal_callbacks = std::move (terminal);
                _terminal_callbacks_tail = tail;
                return;
            }
        }
        [&] () noexcept {
            const auto ambient_guard = enter_ambient_context (snapshot);
            callback (*_result);
        }();
    }
''' + s[b:]
s=s.replace('    std::vector<callback_t> _terminal_callbacks;', '    std::unique_ptr<callback_t> _terminal_callbacks;\n    callback_t *_terminal_callbacks_tail = nullptr;')
s=s.replace('std::function<void (const result_t<T> &)> (std::forward<TCallback> (callback))','std::forward<TCallback> (callback)')
p.write_text(s)
p=root/'src/runtime/protocol/service_wire_codec.cpp';s=p.read_text();s=s.replace('''    std::vector<std::uint8_t> result{magic[0], magic[1], wire_major,
                                     static_cast<std::uint8_t> (command::nodeRequest), 0};
    append_u64 (result, correlation);''','''    std::vector<std::uint8_t> result;
    result.reserve (prefix_size + 8);
    result.insert (result.end (), {magic[0], magic[1], wire_major,
                                   static_cast<std::uint8_t> (command::nodeRequest), 0});
    append_u64 (result, correlation);''');p.write_text(s)
