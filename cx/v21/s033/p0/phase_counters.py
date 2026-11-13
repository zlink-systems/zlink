from pathlib import Path
r=Path('/root/worktree/zlink-v21-s033/p0')
p=r/'framework/languages/cpp/framework/include/zlink/framework/detail/path_counters.hpp';s=p.read_text().replace('#include <array>','#include <array>\n#include <atomic>')
s=s.replace('// Fixed sparse exact histograms:', '''// Only the existing bench phase boundary writes this selector; updates use loads only.
inline std::atomic<unsigned> active_phase{0};
inline unsigned phase() noexcept { return active_phase.load(std::memory_order_relaxed); }
// Fixed sparse exact histograms:''')
s=s.replace('std::array<histogram_t, metric_count> hist{};', 'std::array<std::array<histogram_t, metric_count>,2> hist{};')
s=s.replace('std::array<std::uint64_t, total_count> totals{};', 'std::array<std::array<std::uint64_t, total_count>,2> totals{};')
a=s.index('        std::fprintf(f, "{\\"pid');b=s.index('\n    }\n};\ninline counters_t',a)
s=s[:a]+'''        std::fprintf(f, "{\\"pid\\":%ld,\\"tid\\":%ld,\\"phases\\":[",long(getpid()),long(syscall(SYS_gettid)));
        for(unsigned phase=0;phase<2;++phase) {
            std::fprintf(f,"%s{\\"phase\\":%u,\\"totals\\":{",phase?",":"",phase);
            for (unsigned i=0;i<total_count;++i)
                std::fprintf(f,"%s\\"%s\\":%llu",i?",":"",total_names[i],(unsigned long long)totals[phase][i]);
            std::fputs("},\\"histograms\\":{",f);
            for(unsigned i=0;i<metric_count;++i) {
                const auto &h=hist[phase][i];
                std::fprintf(f,"%s\\"%s\\":{\\"count\\":%llu,\\"sum\\":%llu,\\"max\\":%llu,\\"lost\\":%llu,\\"bins\\":[",i?",":"",metric_names[i],(unsigned long long)h.count,(unsigned long long)h.sum,(unsigned long long)h.maximum,(unsigned long long)h.lost);
                bool comma=false;
                for(const auto &b:h.buckets) if(b.count) {
                    std::fprintf(f,"%s[%llu,%llu]",comma?",":"",(unsigned long long)b.value,(unsigned long long)b.count); comma=true;
                }
                std::fputs("]}",f);
            }
            std::fputs("}}",f);
        }
        std::fputs("]}\\n",f); std::fclose(f);'''+s[b:]
s=s.replace('++local().totals[key]', '++local().totals[phase()][key]')
s=s.replace('local().hist[key].add(value)', 'local().hist[phase()][key].add(value)')
s=s.replace('local().totals[worker_startup_records]', 'local().totals[phase()][worker_startup_records]')
s=s.replace('c.totals[worker_records]', 'c.totals[phase()][worker_records]')
s=s.replace('c.totals[app_notify]', 'c.totals[phase()][app_notify]')
s=s.replace('c.totals[registry_run]', 'c.totals[phase()][registry_run]')
p.write_text(s)
p=r/'framework/bench/grpc/cpp/client/bench_cpp_client.cpp';s=p.read_text();s=s.replace('''        const bool active = phase == "active";
        if (!active)''','''        const bool active = phase == "active";
        ZLINK_PATH_COUNT(zlink::framework::path_counters::active_phase.store(active ? 1u : 0u, std::memory_order_relaxed);)
        if (!active)''',1);p.write_text(s)
p=r/'framework/bench/grpc/cpp/common/bench_stats_server.hpp';s=p.read_text();s='#include <zlink/framework/detail/path_counters.hpp>\n'+s;s=s.replace('''    void reset ()
    {
        std::lock_guard''','''    void reset ()
    {
        ZLINK_PATH_COUNT(zlink::framework::path_counters::active_phase.store(1u, std::memory_order_relaxed);)
        std::lock_guard''',1);p.write_text(s)
