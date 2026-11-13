from pathlib import Path
w=Path('/root/worktree/zlink-xlang/framework/bench/grpc')
p=w/'cpp/client/bench_cpp_client.cpp'
s=p.read_text(); anchor='namespace\n{'; assert anchor in s
helper='''namespace
{
// Measurement-only open-loop pacing; disabled unless XLANG_RATE is set.
void xlang_pace ()
{
    static const uint64_t rate = std::getenv ("XLANG_RATE") ? std::strtoull (std::getenv ("XLANG_RATE"), nullptr, 10) : 0;
    if (!rate) return;
    static std::atomic<uint64_t> next {0};
    const auto now = std::chrono::steady_clock::now ();
    uint64_t zero = 0;
    next.compare_exchange_strong (zero, static_cast<uint64_t> (now.time_since_epoch ().count ()));
    const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration> (std::chrono::nanoseconds (1000000000 / rate)).count ();
    const auto ticket = next.fetch_add (step);
    std::this_thread::sleep_until (std::chrono::steady_clock::time_point (std::chrono::steady_clock::duration (ticket)));
}
'''
s=s.replace(anchor,helper,1)
s=s.replace('const uint64_t seq = _seq++;','xlang_pace ();\n            const uint64_t seq = _seq++;')
s=s.replace('auto payload = make_payload (size, phase);','xlang_pace ();\n                auto payload = make_payload (size, phase);')
p.write_text(s)
p=w/'java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java'
s=p.read_text(); pos=s.index('{',s.index('public final class BenchDrivers'))+1
s=s[:pos]+'''
    // Measurement-only pacing, shared by all submit workers.
    private static final long XLANG_RATE = Long.parseLong(System.getenv().getOrDefault("XLANG_RATE", "0"));
    private static final java.util.concurrent.atomic.AtomicLong XLANG_NEXT = new java.util.concurrent.atomic.AtomicLong();
    private static void xlangPace() {
        if (XLANG_RATE == 0) return;
        XLANG_NEXT.compareAndSet(0, System.nanoTime());
        long ticket = XLANG_NEXT.getAndAdd(1_000_000_000L / XLANG_RATE);
        long remaining;
        while ((remaining = ticket - System.nanoTime()) > 0)
            java.util.concurrent.locks.LockSupport.parkNanos(remaining);
    }
''' + s[pos:]
# Every issuance loop is scoped by its active deadline; pacing precedes begin/encode.
s=s.replace('while (BenchMetricHeader.nowNs() < deadline) {','while (BenchMetricHeader.nowNs() < deadline) {\n            xlangPace();')
p.write_text(s)
p=w/'dotnet/Client/BenchMeasurement.cs';s=p.read_text();pos=s.index('{',s.index('internal static class BenchMeasurement'))+1
s=s[:pos]+'''
    private static readonly long XlangRate = long.Parse(Environment.GetEnvironmentVariable("XLANG_RATE") ?? "0");
    private static long XlangNext;
    [System.Runtime.InteropServices.DllImport("libc", EntryPoint = "nanosleep")]
    private static extern int XlangSleep(ref XlangTimespec requested, nint remaining);
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct XlangTimespec { public long Seconds; public long Nanoseconds; }
    private static void XlangPace()
    {
        if (XlangRate == 0) return;
        Interlocked.CompareExchange(ref XlangNext, Stopwatch.GetTimestamp(), 0);
        long step = Stopwatch.Frequency / XlangRate;
        long ticket = Interlocked.Add(ref XlangNext, step) - step;
        long remaining;
        while ((remaining = ticket - Stopwatch.GetTimestamp()) > 0) {
            long ns = remaining * 1_000_000_000L / Stopwatch.Frequency;
            var delay = new XlangTimespec { Seconds = ns / 1_000_000_000, Nanoseconds = ns % 1_000_000_000 };
            XlangSleep(ref delay, 0);
        }
    }
''' + s[pos:]
s=s.replace('while (Stopwatch.GetTimestamp() < deadline)\n                        {','while (Stopwatch.GetTimestamp() < deadline)\n                        {\n                            XlangPace();')
s=s.replace('while (Stopwatch.GetTimestamp() < deadline)\n        {','while (Stopwatch.GetTimestamp() < deadline)\n        {\n            XlangPace();')
p.write_text(s)
