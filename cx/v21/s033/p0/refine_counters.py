from pathlib import Path
R=Path('/root/worktree/zlink-v21-s033/p0')
p=R/'framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp'
s=p.read_text().replace('!submission.admitted.await_ready()', 'submission.result == ZLINK_SUBMIT_BACKPRESSURED')
p.write_text(s)
p=R/'framework/languages/cpp/framework/include/zlink/framework/detail/path_counters.hpp'
s=p.read_text().replace('application_batch, send_pending','application_batch, dispatcher_application, send_pending')
s=s.replace('"owner_drain", "dispatcher_turn", "application_batch", "send_pending"','"owner_drain", "dispatcher_turn", "application_batch", "dispatcher_application", "send_pending"')
s=s.replace('*completion_records = nullptr;', '*completion_records = nullptr, *completion_application = nullptr;')
s=s.replace('std::uint64_t records=0; std::uint64_t *previous;\n    completion_t():previous(local().completion_records) { local().completion_records=&records; }\n    ~completion_t() { local().completion_records=previous; sample(dispatcher_turn,records); }', '''std::uint64_t records=0, application=0; std::uint64_t *previous, *previous_application;
    completion_t():previous(local().completion_records),previous_application(local().completion_application) {
        local().completion_records=&records; local().completion_application=&application;
    }
    ~completion_t() {
        local().completion_records=previous; local().completion_application=previous_application;
        sample(dispatcher_turn,records); sample(dispatcher_application,application);
    }''')
s=s.replace('inline void completed() noexcept {', '''inline void application_notified() noexcept {
    auto &c=local(); ++c.totals[app_notify];
    if(c.completion_application) ++*c.completion_application;
}
inline void completed() noexcept {''')
p.write_text(s)
p=R/'framework/bench/grpc/cpp/common/bench_async.hpp';s=p.read_text();s=s.replace('''zlink::framework::path_counters::increment(
            slot == std::numeric_limits<size_t>::max() ? zlink::framework::path_counters::app_admission_notify
            : zlink::framework::path_counters::app_notify);''','''if(slot == std::numeric_limits<size_t>::max())
            zlink::framework::path_counters::increment(zlink::framework::path_counters::app_admission_notify);
            else zlink::framework::path_counters::application_notified();''');p.write_text(s)
p=R/'framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp';s=p.read_text();a='''    if (!mailbox.begin_application_drain (owner))
        co_return;
    ZLINK_PATH_COUNT(struct path_drain_t''';b='''    ZLINK_PATH_COUNT(struct path_drain_t''';assert a in s;s=s.replace(a,b,1);a='''    } path_drain;)
    const auto deadline''';b='''    } path_drain;)
    if (!mailbox.begin_application_drain (owner))
        co_return;
    const auto deadline''';s=s.replace(a,b,1);p.write_text(s)
