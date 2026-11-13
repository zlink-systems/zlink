from pathlib import Path
r=Path('/root/worktree/zlink-v21-s033/p0')
p=r/'framework/languages/cpp/framework/include/zlink/framework/detail/path_counters.hpp';s=p.read_text()
s=s.replace('''    ~counters_t() {
        const auto *prefix''','''    counters_t *next = nullptr;
    long tid = 0;
    void dump() const {
        const auto *prefix''')
s=s.replace('long(syscall(SYS_gettid))', 'tid')
s=s.replace('''inline counters_t &local() { static thread_local counters_t counters; return counters; }''','''// Linux TIDs address distinct slots. First touch allocates and publishes once; no
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
};''')
p.write_text(s)
for name in ['framework/bench/grpc/cpp/client/bench_cpp_client.cpp','framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp']:
 p=r/name;s=p.read_text()
 if 'path_counters.hpp' not in s:s='#include <zlink/framework/detail/path_counters.hpp>\n'+s
 a='''int main (int argc, char **argv)
{''';b='''int main (int argc, char **argv)
{
    ZLINK_PATH_COUNT(zlink::framework::path_counters::dump_at_exit_t path_dump;)''';assert a in s;s=s.replace(a,b,1);p.write_text(s)
