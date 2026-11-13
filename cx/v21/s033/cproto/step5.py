from pathlib import Path
r=Path('/root/worktree/zlink-cproto/framework/languages/cpp/framework/src/runtime')
p=r/'mesh/mesh_node_runtime.cpp';s=p.read_text().replace('''        _node->submit_node_request (target, std::move (parts), timeout,
                                    std::move (completion), receiver.admitted);''','''        _node->transport ().submit_node_request (
          {target.data (), target.size ()}, std::move (parts).take_application_payload (),
          timeout, std::move (completion), receiver.admitted);''');p.write_text(s)
p=r/'stateful/public_host_runtime.hpp';s=p.read_text();a=s.index('    void submit_node_request (');b=s.index(';',a)+1;s=s[:a]+s[b:];p.write_text(s)
p=r/'stateful/public_host_runtime.cpp';s=p.read_text();a=s.index('void public_host_runtime_t::submit_node_request (');b=s.index('\ntask_t<zlink::submit_result_t>',a);s=s[:a]+s[b:];p.write_text(s)
p=r/'mesh/mesh_node_runtime.cpp';s=p.read_text()
old='''        _node->transport ().submit_node_request (
          {target.data (), target.size ()}, std::move (parts).take_application_payload (),
          timeout, std::move (completion), receiver.admitted);
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
        // Exercise failure after C1 transfer at the direct admission connection.
        if (auto fault = test_creation_probe::node_admission_observer_fault.exchange (nullptr))
            fault ();
        if (auto fault = test_creation_probe::node_admission_observer_context_fault.exchange (nullptr))
            fault ();
#endif'''
new='''#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
        std::exception_ptr submission_error;
        try {
#endif
        _node->transport ().submit_node_request (
          {target.data (), target.size ()}, std::move (parts).take_application_payload (),
          timeout, std::move (completion), receiver.admitted);
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
        }
        catch (...) {
            submission_error = std::current_exception ();
        }
        // Preserve the post-submission fault point for both submission outcomes.
        if (auto fault = test_creation_probe::node_admission_observer_fault.exchange (nullptr))
            fault ();
        if (auto fault = test_creation_probe::node_admission_observer_context_fault.exchange (nullptr))
            fault ();
        if (submission_error)
            std::rethrow_exception (submission_error);
#endif'''
assert old in s;s=s.replace(old,new,1);p.write_text(s)
