/* Measurement-only instrumentation for #1639 step 0. */
#pragma once
#ifdef ZLINK_FRAMEWORK_CPP_PATH_COUNTERS
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/syscall.h>
namespace zlink::framework::path_counters {
// Only the existing bench phase boundary writes this selector; updates use loads only.
inline std::atomic<unsigned> active_phase{0};
inline unsigned phase() noexcept { return active_phase.load(std::memory_order_relaxed); }
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
enum metric_t { worker_turn, worker_wake, rotation, rotation_activity, owner_drain, dispatcher_turn,
                application_batch, dispatcher_application, send_pending, request_pending, metric_count };
enum total_t { worker_notify, worker_wait, worker_wait_ready, worker_records,
               owner_post, dispatcher_post, dispatcher_run, registry_post, registry_run,
               app_notify, app_admission_notify, app_immediate, app_completed,
               request_snapshot_pending, request_attempt, send_attempt, worker_startup_records, total_count };
inline constexpr const char *metric_names[] = {"worker_turn", "worker_wake", "rotation", "rotation_activity",
    "owner_drain", "dispatcher_turn", "application_batch", "dispatcher_application", "send_pending", "request_pending"};
inline constexpr const char *total_names[] = {"worker_notify", "worker_wait", "worker_wait_ready",
    "worker_records", "owner_post", "dispatcher_post", "dispatcher_run", "registry_post",
    "registry_run", "app_notify", "app_admission_notify", "app_immediate", "app_completed",
    "request_snapshot_pending", "request_attempt", "send_attempt", "worker_startup_records"};
struct counters_t {
    std::array<std::array<histogram_t, metric_count>,2> hist{};
    std::array<std::array<std::uint64_t, total_count>,2> totals{};
    std::uint64_t *turn_records = nullptr, *wake_records = nullptr, *completion_records = nullptr, *completion_application = nullptr;
    counters_t *next = nullptr;
    long tid = 0;
    void dump() const {
        const auto *prefix = std::getenv("ZLINK_PATH_COUNTER_FILE");
        if (!prefix || !*prefix) return;
        char name[4096];
        std::snprintf(name, sizeof(name), "%s.%ld.%ld.%p.json", prefix, long(getpid()), tid, static_cast<const void *>(this));
        auto *f = std::fopen(name, "w"); if (!f) return;
        std::fprintf(f, "{\"pid\":%ld,\"tid\":%ld,\"phases\":[",long(getpid()),tid);
        for(unsigned phase=0;phase<2;++phase) {
            std::fprintf(f,"%s{\"phase\":%u,\"totals\":{",phase?",":"",phase);
            for (unsigned i=0;i<total_count;++i)
                std::fprintf(f,"%s\"%s\":%llu",i?",":"",total_names[i],(unsigned long long)totals[phase][i]);
            std::fputs("},\"histograms\":{",f);
            for(unsigned i=0;i<metric_count;++i) {
                const auto &h=hist[phase][i];
                std::fprintf(f,"%s\"%s\":{\"count\":%llu,\"sum\":%llu,\"max\":%llu,\"lost\":%llu,\"bins\":[",i?",":"",metric_names[i],(unsigned long long)h.count,(unsigned long long)h.sum,(unsigned long long)h.maximum,(unsigned long long)h.lost);
                bool comma=false;
                for(const auto &b:h.buckets) if(b.count) {
                    std::fprintf(f,"%s[%llu,%llu]",comma?",":"",(unsigned long long)b.value,(unsigned long long)b.count); comma=true;
                }
                std::fputs("]}",f);
            }
            std::fputs("}}",f);
        }
        std::fputs("]}\n",f); std::fclose(f);
    }
};
// Linux TIDs address distinct slots. First touch allocates and publishes once; no
// registry lock/RMW is needed. Retired threads retain their counters until all
// bench hosts have stopped and joined. The dump guard is the first main local.
inline std::array<counters_t *, 1u << 22> registry{};
inline counters_t &local() {
    static thread_local counters_t *counters = [] {
        const long tid = long(syscall(SYS_gettid));
        if(tid <= 0 || static_cast<unsigned long>(tid) >= registry.size()) std::abort();
        auto *value = new counters_t;
        value->tid = tid; value->next = registry[tid]; registry[tid] = value;
        return value;
    }();
    return *counters;
}
struct dump_at_exit_t {
    ~dump_at_exit_t() {
        for(auto *&head:registry) while(head) {
            auto *value=head; head=value->next;
            value->dump(); delete value;
        }
    }
};
inline void increment(total_t key) noexcept { ++local().totals[phase()][key]; }
inline void sample(metric_t key,std::uint64_t value) noexcept { local().hist[phase()][key].add(value); }
struct turn_t {
    std::uint64_t records=0; std::uint64_t *previous; bool enabled;
    explicit turn_t(bool enabled):previous(nullptr),enabled(enabled) {
        if(enabled) { previous=local().turn_records; local().turn_records=&records; }
    }
    ~turn_t() { if(enabled) { local().turn_records=previous; sample(worker_turn,records); } }
};
struct wake_t {
    std::uint64_t records=0; bool active=false, enabled;
    explicit wake_t(bool enabled):enabled(enabled) { if(enabled) local().wake_records=&records; }
    void sleep() {
        if(!enabled) return;
        if(active) sample(worker_wake,records); else local().totals[phase()][worker_startup_records]+=records;
        active=false; records=0;
    }
    void woke(bool ready) { if(enabled && ready) { increment(worker_wait_ready); active=true; } }
    ~wake_t() { if(enabled) { sleep(); local().wake_records=nullptr; } }
};
struct completion_t {
    std::uint64_t records=0, application=0; std::uint64_t *previous, *previous_application;
    completion_t():previous(local().completion_records),previous_application(local().completion_application) {
        local().completion_records=&records; local().completion_application=&application;
    }
    ~completion_t() {
        local().completion_records=previous; local().completion_application=previous_application;
        sample(dispatcher_turn,records); sample(dispatcher_application,application);
    }
};
inline void record() noexcept {
    auto &c=local(); ++c.totals[phase()][worker_records];
    if(c.turn_records) ++*c.turn_records;
    if(c.wake_records) ++*c.wake_records;
}
inline void application_notified() noexcept {
    auto &c=local(); ++c.totals[phase()][app_notify];
    if(c.completion_application) ++*c.completion_application;
}
inline void completed() noexcept {
    auto &c=local(); ++c.totals[phase()][registry_run];
    if(c.completion_records) ++*c.completion_records;
}
}
#define ZLINK_PATH_COUNT(...) __VA_ARGS__
#else
#define ZLINK_PATH_COUNT(...)
#endif
