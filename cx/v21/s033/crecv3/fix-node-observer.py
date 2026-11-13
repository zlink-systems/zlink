from pathlib import Path
p=Path('/root/worktree/zlink-3867f/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp')
s=p.read_text();a=s.index('void mesh_node_runtime_t::submit_node_request_reply (');b=s.index('\nvoid mesh_node_runtime_t::request_node_reply (',a);seg=s[a:b]
seg=seg.replace('''    auto completion = runtime::foundation::operation_registry_t::callback_t (
''','''    runtime::foundation::operation_registry_t::callback_t completion;
    try {
        completion = runtime::foundation::operation_registry_t::callback_t (
''',1)
seg=seg.replace('''        }));
    auto submission''','''        }));
    // The stored callback now owns this receiver, including submission failures
    // before registration. Keep no independent physical fallback after transfer.
    receiver.complete = {};
    auto submission''',1)
seg=seg.replace('[admitted = std::move (receiver.admitted), completion = std::move (completion)]','[admitted = receiver.admitted, completion = std::move (completion)]',1)
# The function's existing closing brace now closes the try block first.
pos=seg.rfind('\n}')
seg=seg[:pos]+'''
    }
    catch (...) {
        const auto failure = std::current_exception ();
        receiver.admitted->complete (result_access_t::failure<void> (failure));
        if (completion)
            completion (runtime::foundation::operation_terminal_t::transport_failed,
                        result_access_t::failure<zlink::message_t> (failure));
        else if (receiver.complete)
            receiver.complete (result_access_t::failure<node_reply_t> (failure));
    }
}'''+seg[pos+2:]
s=s[:a]+seg+s[b:];p.write_text(s)
