from pathlib import Path
root=Path('/root/worktree/zlink-3867r/framework/languages/cpp/framework')
p=root/'include/zlink/framework/contracts/dispatch/request_task.hpp';s=p.read_text()
s=s.replace('request_callback_t (std::shared_ptr<TOwner> owner, TInvoke) :\n        _owner', 'request_callback_t (std::shared_ptr<TOwner> owner, TInvoke,\n                        request_capture_storage_t *storage = nullptr) :\n        _storage (storage), _owner',1)
s=s.replace('        auto selected = *this;\n        selected._owner', '        auto selected = *this;\n        if (!_storage)\n            return selected;\n        selected._owner',1)
s=s.replace('    request_callback_t<TNextArgs...> capture (TCallback callback) const\n    {\n        if (!_storage)\n            return request_callback_t<TNextArgs...> (std::move (callback));\n        auto *stored = _storage->store (std::move (callback));\n        return request_callback_t<TNextArgs...>::from_storage (_owner, _storage, stored);\n    }', '''    request_callback_t<TNextArgs...> capture (TCallback callback, TCallback **address = nullptr) const
    {
        if (!_storage) {
            auto stored = std::make_shared<TCallback> (std::move (callback));
            if (address)
                *address = stored.get ();
            return request_callback_t<TNextArgs...>::from_shared (std::move (stored));
        }
        auto *stored = _storage->store (std::move (callback));
        if (address)
            *address = stored;
        return request_callback_t<TNextArgs...>::from_storage (_owner, _storage, stored);
    }

    bool has_request_storage () const noexcept { return _storage != nullptr; }''',1)
s=s.replace('        return {std::shared_ptr<task_completion_source_t<void>> (\n                  std::shared_ptr<task_completion_source_t<void>>{}, admitted.get ()),','        return {complete.has_request_storage ()\n                  ? std::shared_ptr<task_completion_source_t<void>> (\n                      std::shared_ptr<task_completion_source_t<void>>{}, admitted.get ())\n                  : admitted,',1)
s=s.replace('              reply.complete (result_access_t::failure<T> (std::move (failure)));\n          })};','              reply.complete (result_access_t::failure<T> (std::move (failure)));\n          }, storage.get ())};',1)
p.write_text(s)
p=root/'src/runtime/channels/channel_runtime.cpp';s=p.read_text()
for typ in ['node_reply_t','reply_parts_t']:
 old='''            auto context = std::make_shared<projection_t> (
              projection_t{std::move (receiver.complete), std::move (decode)});
            detail::request_receiver_t<'''+typ+'''> physical{
              std::move (receiver.admitted),
              detail::request_callback_t<const result_t<'''+typ+'''> &>::from_shared (context),
              std::move (receiver.fail_public)};'''
 new='''            projection_t *context = nullptr;
            auto complete = receiver.complete.template capture<const result_t<'''+typ+'''> &> (
              projection_t{receiver.complete.borrow (), std::move (decode)}, &context);
            detail::request_receiver_t<'''+typ+'''> physical{
              std::move (receiver.admitted), std::move (complete), std::move (receiver.fail_public)};'''
 assert old in s,typ
 s=s.replace(old,new,1)
p.write_text(s)
p=root/'src/runtime/mesh/mesh_node_runtime.cpp';s=p.read_text();a=s.index('void mesh_node_runtime_t::submit_node_request_reply');b=s.index('\nvoid mesh_node_runtime_t::request_node_reply',a);part=s[a:b]
part=part.replace('    auto submission = _node->request_to_node (\n      target, std::move (parts), timeout,\n      [host = _node, receiver]', '    auto completion = receiver.complete.template capture<runtime::foundation::operation_terminal_t,\n                                                          result_t<zlink::message_t>> (\n      [host = _node, receiver = receiver.borrow ()]',1)
part=part.replace('      },\n      receiver.admitted);', '      });\n    auto submission = _node->request_to_node (\n      target, std::move (parts), timeout,\n      runtime::foundation::operation_registry_t::callback_t (std::move (completion)),\n      receiver.admitted);',1)
s=s[:a]+part+s[b:];p.write_text(s)
# completion_sink must preserve the existing request callback instead of boxing it again.
p=root/'src/runtime/foundation/operation_registry.hpp';s=p.read_text().replace('    completion_sink_t (std::nullptr_t) {}','    completion_sink_t (std::nullptr_t) {}\n    completion_sink_t (detail::request_callback_t<operation_terminal_t, result_t<zlink::message_t>> complete) :\n        _complete (std::move (complete))\n    {\n    }',1);p.write_text(s)
