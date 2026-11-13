from pathlib import Path
R=Path('/root/worktree/zlink-v21-s033/p0')
def edit(path,a,b):
 p=R/path;s=p.read_text();assert a in s,path;p.write_text(s.replace(a,b,1))
h=R/'framework/languages/cpp/framework/include/zlink/framework/detail/path_counters.hpp'
h.write_text('''/* Measurement-only instrumentation for #1639 step 0. */
#pragma once
#ifdef ZLINK_FRAMEWORK_CPP_PATH_COUNTERS
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/syscall.h>
namespace zlink::framework::path_counters {
// Fixed sparse exact histograms: no allocation, synchronization, clock or I/O on update.
struct histogram_t {
    struct bucket_t { std::uint64_t value = 0, count = 0; };
    std::array<bucket_t, 8192> buckets{};
    std::uint64_t count = 0, sum = 0, maximum = 0, lost = 0;
    void add(std::uint64_t value) noexcept {
        ++count; sum += value; if (maximum < value) maximum = value;
        auto slot = (value * 11400714819323198485ull) >> (64 - 13);
        for (unsigned i = 0; i < buckets.size(); ++i) {
            auto &b = buckets[(slot + i) & (buckets.size() - 1)];
            if (!b.count || b.value == value) { b.value = value; ++b.count; return; }
        }
        ++lost;
    }
};
enum metric_t { worker_turn, worker_wake, rotation, owner_drain, dispatcher_turn,
                application_batch, send_pending, request_pending, metric_count };
enum total_t { worker_notify, worker_wait, worker_wait_ready, worker_records,
               owner_post, dispatcher_post, dispatcher_run, registry_post, registry_run,
               app_notify, app_admission_notify, app_immediate, app_completed,
               request_snapshot_pending, request_attempt, send_attempt, total_count };
inline constexpr const char *metric_names[] = {"worker_turn", "worker_wake", "rotation",
    "owner_drain", "dispatcher_turn", "application_batch", "send_pending", "request_pending"};
inline constexpr const char *total_names[] = {"worker_notify", "worker_wait", "worker_wait_ready",
    "worker_records", "owner_post", "dispatcher_post", "dispatcher_run", "registry_post",
    "registry_run", "app_notify", "app_admission_notify", "app_immediate", "app_completed",
    "request_snapshot_pending", "request_attempt", "send_attempt"};
struct counters_t {
    std::array<histogram_t, metric_count> hist{};
    std::array<std::uint64_t, total_count> totals{};
    std::uint64_t *turn_records = nullptr, *wake_records = nullptr, *completion_records = nullptr;
    ~counters_t() {
        const auto *prefix = std::getenv("ZLINK_PATH_COUNTER_FILE");
        if (!prefix || !*prefix) return;
        char name[4096];
        std::snprintf(name, sizeof(name), "%s.%ld.%ld.json", prefix, long(getpid()), long(syscall(SYS_gettid)));
        auto *f = std::fopen(name, "w"); if (!f) return;
        std::fprintf(f, "{\\"pid\\":%ld,\\"tid\\":%ld,\\"totals\\":{",long(getpid()),long(syscall(SYS_gettid)));
        for (unsigned i=0;i<total_count;++i)
            std::fprintf(f,"%s\\"%s\\":%llu",i?",":"",total_names[i],(unsigned long long)totals[i]);
        std::fputs("},\\"histograms\\":{",f);
        for(unsigned i=0;i<metric_count;++i) {
            const auto &h=hist[i];
            std::fprintf(f,"%s\\"%s\\":{\\"count\\":%llu,\\"sum\\":%llu,\\"max\\":%llu,\\"lost\\":%llu,\\"bins\\":[",i?",":"",metric_names[i],(unsigned long long)h.count,(unsigned long long)h.sum,(unsigned long long)h.maximum,(unsigned long long)h.lost);
            bool comma=false;
            for(const auto &b:h.buckets) if(b.count) {
                std::fprintf(f,"%s[%llu,%llu]",comma?",":"",(unsigned long long)b.value,(unsigned long long)b.count); comma=true;
            }
            std::fputs("]}",f);
        }
        std::fputs("}}\\n",f); std::fclose(f);
    }
};
inline counters_t &local() { static thread_local counters_t counters; return counters; }
inline void increment(total_t key) noexcept { ++local().totals[key]; }
inline void sample(metric_t key,std::uint64_t value) noexcept { local().hist[key].add(value); }
struct turn_t {
    std::uint64_t records=0; std::uint64_t *previous;
    turn_t():previous(local().turn_records) { local().turn_records=&records; }
    ~turn_t() { local().turn_records=previous; sample(worker_turn,records); }
};
struct wake_t {
    std::uint64_t records=0; bool active=false;
    wake_t() { local().wake_records=&records; }
    void sleep() { if(active) sample(worker_wake,records); active=false; records=0; }
    void woke(bool ready) { if(ready) { increment(worker_wait_ready); active=true; } }
    ~wake_t() { sleep(); local().wake_records=nullptr; }
};
struct completion_t {
    std::uint64_t records=0; std::uint64_t *previous;
    completion_t():previous(local().completion_records) { local().completion_records=&records; }
    ~completion_t() { local().completion_records=previous; sample(dispatcher_turn,records); }
};
inline void record() noexcept {
    auto &c=local(); ++c.totals[worker_records];
    if(c.turn_records) ++*c.turn_records;
    if(c.wake_records) ++*c.wake_records;
}
inline void completed() noexcept {
    auto &c=local(); ++c.totals[registry_run];
    if(c.completion_records) ++*c.completion_records;
}
}
#define ZLINK_PATH_COUNT(...) __VA_ARGS__
#else
#define ZLINK_PATH_COUNT(...)
#endif
''')
base='framework/languages/cpp/'
edit(base+'CMakeLists.txt','option(ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE', '''option(ZLINK_FRAMEWORK_CPP_PATH_COUNTERS "Measurement-only C++ path counters" OFF)
if(ZLINK_FRAMEWORK_CPP_PATH_COUNTERS)
  add_compile_definitions(ZLINK_FRAMEWORK_CPP_PATH_COUNTERS)
endif()
option(ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE''')
edit(base+'CMakeLists.txt','add_library(zlink::framework_provider_abstractions ALIAS', '''if(ZLINK_FRAMEWORK_CPP_PATH_COUNTERS)
  target_compile_definitions(zlink_framework_provider_abstractions PUBLIC ZLINK_FRAMEWORK_CPP_PATH_COUNTERS)
endif()
add_library(zlink::framework_provider_abstractions ALIAS''')
def incfile(path):
 p=R/path;s=p.read_text();s='#include <zlink/framework/detail/path_counters.hpp>\n'+s;p.write_text(s)
rt=base+'framework/src/runtime/'
for f in ['backend/raw_route_port.cpp','backend/raw_binding_adapter.hpp','dispatch/offload_executor.cpp','dispatch/provider_task_context.cpp','foundation/operation_registry.cpp','mesh/mesh_node_host_service.cpp']:
 incfile(rt+f)
edit(rt+'backend/raw_route_port.cpp','auto submission = std::move (operation).async ();\n            pending', '''ZLINK_PATH_COUNT(path_counters::increment(path_counters::send_attempt);)
            auto submission = std::move (operation).async ();
            ZLINK_PATH_COUNT(path_counters::sample(path_counters::send_pending,
                submission.result == ZLINK_SUBMIT_BACKPRESSURED);)
            pending''')
edit(rt+'backend/raw_binding_adapter.hpp','auto submission = std::forward<TSubmit> (submit) ();','''ZLINK_PATH_COUNT(path_counters::increment(path_counters::request_attempt);)
    auto submission = std::forward<TSubmit> (submit) ();
    ZLINK_PATH_COUNT(path_counters::sample(path_counters::request_pending,
        !submission.admitted.await_ready());
        if(submission.result == ZLINK_SUBMIT_BACKPRESSURED)
            path_counters::increment(path_counters::request_snapshot_pending);)''')
p=R/(rt+'dispatch/offload_executor.cpp');s=p.read_text();s=s.replace('    _ready.notify_one ();','    ZLINK_PATH_COUNT(path_counters::increment(path_counters::worker_notify);)\n    _ready.notify_one ();');p.write_text(s)
edit(rt+'dispatch/offload_executor.cpp','    while (true) {\n        work_item_t work;', '''    ZLINK_PATH_COUNT(path_counters::wake_t path_wake;)
    while (true) {
        work_item_t work;''')
edit(rt+'dispatch/offload_executor.cpp','            ++_idle_workers;\n            bool ready', '''            ZLINK_PATH_COUNT(const bool path_wait = !_stopping && _roles.empty() && _queue.empty();
                if(path_wait) { path_wake.sleep(); path_counters::increment(path_counters::worker_wait); })
            ++_idle_workers;
            bool ready''')
edit(rt+'dispatch/offload_executor.cpp','            --_idle_workers;','''            ZLINK_PATH_COUNT(if(path_wait) path_wake.woke(ready);)
            --_idle_workers;''')
edit(rt+'dispatch/offload_executor.cpp','        } else {\n            run_application_turn', '''        } else {
            ZLINK_PATH_COUNT(path_counters::turn_t path_turn;)
            run_application_turn''')
edit(rt+'dispatch/provider_task_context.cpp','        boost::asio::post (_context, std::forward<Work> (work));','''        ZLINK_PATH_COUNT(path_counters::increment(path_counters::dispatcher_post);)
        boost::asio::post (_context, std::forward<Work> (work));''')
edit(rt+'dispatch/provider_task_context.cpp','    dispatcher->post ([work = std::move (work), owner, dispatcher] {','''    dispatcher->post ([work = std::move (work), owner, dispatcher] {
        ZLINK_PATH_COUNT(path_counters::increment(path_counters::dispatcher_run);
            path_counters::completion_t path_completion;)''')
edit(rt+'foundation/operation_registry.cpp','        detail::dispatch_completion (\n          [item = completion.get ()] {','''        ZLINK_PATH_COUNT(path_counters::increment(path_counters::registry_post);)
        detail::dispatch_completion (
          [item = completion.get ()] {
              ZLINK_PATH_COUNT(path_counters::completed();)''')
edit(rt+'mesh/mesh_node_host_service.cpp','        auto ready = mailbox.take_application_receive_turn ();','''        ZLINK_PATH_COUNT(path_counters::sample(path_counters::rotation,count);)
        auto ready = mailbox.take_application_receive_turn ();''')
edit(rt+'mesh/mesh_node_host_service.cpp','                  auto start = make_application_owner_work (node, registration, owner);','''                  ZLINK_PATH_COUNT(path_counters::increment(path_counters::owner_post);)
                  auto start = make_application_owner_work (node, registration, owner);''')
edit(rt+'mesh/mesh_node_host_service.cpp','    const bool admitted = static_cast<bool> (application_permit);','''    ZLINK_PATH_COUNT(if(record.kind == host::record_kind_t::node_send ||
        record.kind == host::record_kind_t::node_request) path_counters::record();)
    const bool admitted = static_cast<bool> (application_permit);''')
# A per-drain local survives coroutine suspension and is sampled on its finishing thread.
edit(rt+'mesh/mesh_node_host_service.cpp','    const auto deadline = std::chrono::steady_clock::now () + dispatch_limits::owner_time_budget;','''    ZLINK_PATH_COUNT(struct path_drain_t {
        std::uint64_t records=0;
        ~path_drain_t() { path_counters::sample(path_counters::owner_drain,records); }
    } path_drain;)
    const auto deadline = std::chrono::steady_clock::now () + dispatch_limits::owner_time_budget;''')
edit(rt+'mesh/mesh_node_host_service.cpp','              dispatch_application (owned_node, owned_registration, ready, record,','''              ZLINK_PATH_COUNT(++path_drain.records;)
              dispatch_application (owned_node, owned_registration, ready, record,''')
# Bench boundary distinguishes immediate sends, admission-only signals and real completions.
b='framework/bench/grpc/cpp/common/bench_async.hpp';incfile(b)
edit(b,'        if (task.await_ready ())\n            completed.push_back (slot);','''        if (task.await_ready ()) {
            ZLINK_PATH_COUNT(zlink::framework::path_counters::increment(zlink::framework::path_counters::app_immediate);)
            completed.push_back (slot);
        }''')
edit(b,'        _ready.notify_one ();','''        ZLINK_PATH_COUNT(zlink::framework::path_counters::increment(
            slot == std::numeric_limits<size_t>::max() ? zlink::framework::path_counters::app_admission_notify
            : zlink::framework::path_counters::app_notify);)
        _ready.notify_one ();''')
edit(b,'        _completed.clear ();','''        _completed.clear ();
        ZLINK_PATH_COUNT(std::uint64_t count=0;
            for(auto slot:completed) if(slot != std::numeric_limits<size_t>::max()) ++count;
            zlink::framework::path_counters::sample(zlink::framework::path_counters::application_batch,count);)''')
f='framework/bench/grpc/cpp/client/bench_cpp_client.cpp';incfile(f)
edit(f,'                auto &item = *pending[slot];','''                ZLINK_PATH_COUNT(zlink::framework::path_counters::increment(zlink::framework::path_counters::app_completed);)
                auto &item = *pending[slot];''')
print('placed counters')
