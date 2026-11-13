from pathlib import Path
base=Path('/root/worktree/zlink-3867r/framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch')
s=Path('/root/jobs/fx/cx/v21/s033/crecv/task.hpp.before').read_text()
s=s.replace('template <typename T> class task_shared_state_t;', 'struct request_task_access_t;\ntemplate <typename T> class task_shared_state_t;',1)
s=s.replace('    virtual ~task_shared_state_t () = default;', '''    virtual ~task_shared_state_t () = default;

    // Embedded request stages alias the request allocation. Ordinary tasks keep
    // the shared ownership established by their own allocation.
    virtual std::shared_ptr<task_shared_state_t> shared_from_this ()
    {
        return std::enable_shared_from_this<task_shared_state_t>::shared_from_this ();
    }
    virtual std::weak_ptr<task_shared_state_t> weak_from_this () noexcept
    {
        return std::enable_shared_from_this<task_shared_state_t>::weak_from_this ();
    }''',1)
s=s.replace('        [&] () noexcept {\n            for (auto *callback = terminal_callbacks.get ();', '        [&] () noexcept {\n            publish_terminal (*_result);\n            for (auto *callback = terminal_callbacks.get ();',1)
s=s.replace('                owner_state = this->weak_from_this ();','                owner_state = self;',1)
s=s.replace('  protected:\n    virtual void bind_wait_owner', '  protected:\n    virtual void publish_terminal (const result_t<T> &) {}\n\n    virtual void bind_wait_owner',1)
s=s.replace('    // The shared-source allocation owns this wrapper and state.', '    friend struct detail::request_task_access_t;\n\n    // The shared-source allocation owns this wrapper and state.',1)
s=s.replace('    friend class task_completion_source_t<T>;', '    friend struct detail::request_task_access_t;\n    friend class task_completion_source_t<T>;',1)
s=s.replace('    friend class task_completion_source_t<void>;', '    friend struct detail::request_task_access_t;\n    friend class task_completion_source_t<void>;',1)
(base/'task.hpp').write_text(s)
p=base/'request_task.hpp';s=p.read_text().replace('#include <cstddef>','#include <cstddef>\n#include <memory_resource>',1)
s=s.replace('''            std::pmr::memory_resource *terminal_resource () noexcept override
            {
                return owner.resource ();
            }
''','',1)
s=s.replace('        std::pmr::memory_resource *terminal_resource () noexcept override { return resource (); }','''        void publish_terminal (const result_t<void> &result) override
        {
            if constexpr (Public) {
                if (!result)
                    reply.complete (result_access_t::failure<T> (result.exception ()));
            }
        }''',1)
a=s.index('            // The admission observer is embedded in this block')
b=s.index('\n        }\n        request_receiver_t<TInput>',a)
s=s[:a]+s[b:]
# Only capture storage uses this resource; no external allocation customization is exposed.
s=s.replace('    std::pmr::memory_resource *resource () noexcept { return &_resource; }\n','',1)
p.write_text(s)
