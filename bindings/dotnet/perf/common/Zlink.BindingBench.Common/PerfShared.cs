using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using Systems.Zlink;

public static class PerfShared
{
    public const int ErrnoEintr = 4;
    public const int ErrnoEagain = 11;
    public const int ErrnoEintrWin = 10004;
    public const int ErrnoEagainAlt = 35;
    public const int ErrnoEagainWin = 10035;
    public const int ErrnoEtimedout = 110;
    public const int ErrnoEtimedoutAlt = 60;
    public const int ErrnoEtimedoutWin = 10060;
    public const uint PerfMetricMagic = 0x5A4C_4E4Bu;
    public const int PerfMetricHeaderSize = 29;
    private static int _nextPort = InitializePortSeed();
    private static readonly long EpochBaseTimestamp = Stopwatch.GetTimestamp();
    private static readonly ulong EpochBaseNs = (ulong)(DateTime.UtcNow.Ticks * 100L);
    private static readonly double StopwatchTickNs =
        1_000_000_000.0 / Stopwatch.Frequency;

    public static long TimestampNs()
    {
        long ts = Stopwatch.GetTimestamp();
        return (long)(ts * (1_000_000_000.0 / Stopwatch.Frequency));
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static ulong EpochNs()
    {
        return (ulong)(DateTime.UtcNow.Ticks * 100L);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static ulong EpochNsFromTimestamp(long timestamp)
    {
        long deltaTicks = timestamp - EpochBaseTimestamp;
        ulong deltaNs = (ulong)(deltaTicks * StopwatchTickNs);
        return EpochBaseNs + deltaNs;
    }

    public static long DeadlineTicksFromMilliseconds(int milliseconds)
    {
        long boundedMs = Math.Max(1, milliseconds);
        long deltaTicks = (boundedMs * Stopwatch.Frequency) / 1000;
        if (deltaTicks <= 0)
            deltaTicks = 1;
        return Stopwatch.GetTimestamp() + deltaTicks;
    }

    public static long DeadlineTicksFromSeconds(int seconds)
    {
        int boundedSeconds = Math.Max(1, seconds);
        return Stopwatch.GetTimestamp()
            + ((long)boundedSeconds * Stopwatch.Frequency);
    }

    public static double ElapsedSecondsFromTicks(long startTicks, long endTicks)
    {
        long deltaTicks = Math.Max(0, endTicks - startTicks);
        return deltaTicks / (double)Stopwatch.Frequency;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void ReservoirSample(List<double> samples, double value,
        ref long seenCount, int cap, ref uint rngState)
    {
        // C perf keeps every valid active latency sample and computes exact
        // percentiles over that set. The cap/rng parameters are kept for call
        // site compatibility but do not limit the official perf surface.
        _ = cap;
        _ = rngState;
        samples.Add(value);
        seenCount++;
    }

    public static (double mean, double p95, double p99) ComputeLatencyStats(
        List<double> samples)
    {
        if (samples.Count == 0)
            return (0.0, 0.0, 0.0);

        double sum = 0.0;
        for (int i = 0; i < samples.Count; i++)
            sum += samples[i];

        samples.Sort();
        return (sum / samples.Count, PercentileFromSorted(samples, 0.95),
            PercentileFromSorted(samples, 0.99));
    }

    private static double PercentileFromSorted(IReadOnlyList<double> sorted,
        double q)
    {
        if (sorted.Count == 0)
            return 0.0;
        if (q <= 0.0)
            return sorted[0];
        if (q >= 1.0)
            return sorted[^1];

        double pos = (sorted.Count - 1) * q;
        int lo = (int)pos;
        int hi = lo + 1 < sorted.Count ? lo + 1 : lo;
        double frac = pos - lo;
        return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
    }

    public static string EndpointFor(string transport, string name)
    {
        if (transport == "inproc")
            return $"inproc://bench-{name}-{Guid.NewGuid()}";

        if (transport == "ipc")
        {
            string path = $"/tmp/zlink-bench-{name}-{Guid.NewGuid():N}.sock";
            TryDeleteFile(path);
            AppDomain.CurrentDomain.ProcessExit += (_, _) => TryDeleteFile(path);
            return $"ipc://{path}";
        }

        return $"{transport}://127.0.0.1:*";
    }

    private static double NsToMs(double latencyNs)
    {
        return latencyNs / 1_000_000.0;
    }

    public static void PrintResult(string pattern, string transport, int size,
        double throughput, double latencyNs, double latencyP95Ns,
        double latencyP99Ns, double bandwidthMultiplier, bool fixedFormat)
    {
        double bandwidth = (throughput * size * bandwidthMultiplier) / 1_000_000.0;
        WriteMetric("throughput", throughput);
        WriteMetric("bandwidth", bandwidth);
        WriteMetric("latency", NsToMs(latencyNs));
        WriteMetric("latency_p95", NsToMs(latencyP95Ns));
        WriteMetric("latency_p99", NsToMs(latencyP99Ns));
        return;

        void WriteMetric(string metric, double value)
        {
            string formatted = fixedFormat
                ? value.ToString("F3", CultureInfo.InvariantCulture)
                : value.ToString(CultureInfo.InvariantCulture);
            WriteStdoutLine(
                $"RESULT,dotnet,{pattern},{transport},{size},{metric},{formatted}");
        }
    }

    public static int PrintUnsupported(string pattern, string transport,
        int size, string reason)
    {
        _ = size;
        _ = reason;
        WriteStdoutLine($"UNSUPPORTED,dotnet,{pattern},{transport}");
        return 0;
    }

    private static string SingleAutoHwmRoleName(uint role)
    {
        // Mirrors bench_common_runtime.hpp single_auto_hwm_role_name so the
        // dotnet AUTO_HWM_DETAIL line is byte-identical to the C benchmark.
        return role switch
        {
            1u => "control",
            2u => "routed",
            3u => "fanout",
            4u => "recv_ingress",
            5u => "spot_data",
            6u => "peer_queue",
            7u => "stream",
            _ => "none",
        };
    }

    private static bool SingleAutoHwmStatusVisible(MonitorStatus s)
    {
        return s.AutoHwmAppliedSendHighWaterMarkBytes > 0
            || s.AutoHwmAppliedReceiveHighWaterMarkBytes > 0
            || s.AutoHwmEffectiveMessageBytes > 0
            || s.AutoHwmSocketMessageSlots > 0;
    }

    // Byte-identical port of bench_common_runtime.hpp
    // emit_single_socket_hwm_detail. The single report emitter
    // (run_emit.py) parses this AUTO_HWM_DETAIL line exactly like the C
    // single canonical emitter parses the C benchmark output, so the
    // "## Auto-HWM Detail" report block matches the C single report.
    public static void EmitSingleAutoHwmDetail(ISocketMonitor monitor,
        string pattern, string transport, string component,
        string socketType, int msgSize)
    {
        if (monitor is null)
            return;
        MonitorStatus snapshot;
        try
        {
            snapshot = monitor.Status();
        }
        catch
        {
            return;
        }
        if (snapshot is null || !SingleAutoHwmStatusVisible(snapshot))
            return;

        WriteStdoutLine(
            "AUTO_HWM_DETAIL"
            + $",pattern={pattern}"
            + $",transport={transport}"
            + $",component={component}"
            + $",msg_size={msgSize}"
            + ",owner=socket"
            + ",owner_id=0"
            + $",socket={component}"
            + $",socket_type={socketType}"
            + $",role={SingleAutoHwmRoleName(snapshot.AutoHwmRole)}"
            + $",sndhwm={snapshot.AutoHwmAppliedSendHighWaterMarkBytes}"
            + $",rcvhwm={snapshot.AutoHwmAppliedReceiveHighWaterMarkBytes}"
            + $",effective_message_bytes={snapshot.AutoHwmEffectiveMessageBytes}"
            + $",effective_sndbuf={snapshot.AutoHwmEffectiveSndbuf}"
            + $",effective_rcvbuf={snapshot.AutoHwmEffectiveRcvbuf}"
            + $",socket_message_slots={snapshot.AutoHwmSocketMessageSlots}");
    }

    public static void WriteStdoutLine(string line)
    {
        Console.WriteLine(line);
        Console.Out.Flush();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static bool StampMetricHeader(Span<byte> payload, uint runId,
        uint phase, int msgSize, ulong seq, ulong sentTsNs)
    {
        if (payload.Length < PerfMetricHeaderSize)
            return false;

        ref byte head = ref MemoryMarshal.GetReference(payload);
        Unsafe.WriteUnaligned(ref head, PerfMetricMagic);
        Unsafe.WriteUnaligned(ref Unsafe.Add(ref head, 4), runId);
        Unsafe.Add(ref head, 8) = checked((byte)phase);
        Unsafe.WriteUnaligned(ref Unsafe.Add(ref head, 9),
            (uint)Math.Max(0, msgSize));
        Unsafe.WriteUnaligned(ref Unsafe.Add(ref head, 13), seq);
        Unsafe.WriteUnaligned(ref Unsafe.Add(ref head, 21),
            checked((long)sentTsNs));
        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static bool TryDecodeMetricHeader(ReadOnlySpan<byte> payload,
        out PerfMetricHeader header)
    {
        header = default;
        if (payload.Length < PerfMetricHeaderSize)
            return false;

        ref byte head = ref MemoryMarshal.GetReference(payload);
        uint magic = Unsafe.ReadUnaligned<uint>(ref head);
        uint runId = Unsafe.ReadUnaligned<uint>(ref Unsafe.Add(ref head, 4));
        uint phase = Unsafe.Add(ref head, 8);
        uint msgSize = Unsafe.ReadUnaligned<uint>(ref Unsafe.Add(ref head, 9));
        ulong seq = Unsafe.ReadUnaligned<ulong>(ref Unsafe.Add(ref head, 13));
        ulong sentTsNs = checked((ulong)Unsafe.ReadUnaligned<long>(
            ref Unsafe.Add(ref head, 21)));

        header = new PerfMetricHeader(magic, runId, phase, msgSize, seq,
            sentTsNs);
        return magic == PerfMetricMagic;
    }

    public static bool IsWouldBlock(int errno)
    {
        return errno == ErrnoEagain
            || errno == ErrnoEagainAlt
            || errno == ErrnoEagainWin;
    }

    public static bool IsInterrupted(int errno)
    {
        return errno == ErrnoEintr || errno == ErrnoEintrWin;
    }

    public static bool IsTimedOut(int errno)
    {
        return errno == ErrnoEtimedout
            || errno == ErrnoEtimedoutAlt
            || errno == ErrnoEtimedoutWin;
    }

    public static bool IsTransientBackpressure(int errno)
    {
        return IsInterrupted(errno) || IsWouldBlock(errno) || IsTimedOut(errno);
    }

    public static bool IsTransientNetworkError(int errno)
    {
        return errno == 51
            || errno == 57
            || errno == 61
            || errno == 65
            || errno == 107
            || errno == 111
            || errno == 113
            || errno == 10051
            || errno == 10057
            || errno == 10061
            || errno == 10065;
    }

    public static void TryDisposeQuietly(IDisposable? disposable)
    {
        if (disposable == null)
            return;

        try
        {
            disposable.Dispose();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[perf-shared] dispose failed: {ex.Message}");
        }
    }

    public static string NormalizePattern(string pattern,
        bool trimMultiPrefix = false)
    {
        string normalized = (pattern ?? string.Empty).Trim().ToUpperInvariant();
        const string multiPrefix = "MULTI_";
        if (trimMultiPrefix
            && normalized.StartsWith(multiPrefix, StringComparison.Ordinal))
        {
            return normalized.Substring(multiPrefix.Length);
        }

        return normalized;
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            if (File.Exists(path))
                File.Delete(path);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[perf-shared] delete file failed: {ex.Message}");
        }
    }

    private static int InitializePortSeed()
    {
        return Random.Shared.Next(20000, 40001);
    }

    private static int GetPort()
    {
        while (true)
        {
            int next = Interlocked.Increment(ref _nextPort);
            if (next <= 59999)
                return next;

            int reset = InitializePortSeed();
            Interlocked.CompareExchange(ref _nextPort, reset, next);
        }
    }

}

public readonly struct PerfMetricHeader
{
    public PerfMetricHeader(uint magic, uint runId, uint phase, uint msgSize,
        ulong seq, ulong sentTsNs)
    {
        Magic = magic;
        RunId = runId;
        Phase = phase;
        MsgSize = msgSize;
        Seq = seq;
        SentTsNs = sentTsNs;
    }

    public uint Magic { get; }
    public uint RunId { get; }
    public uint Phase { get; }
    public uint MsgSize { get; }
    public ulong Seq { get; }
    public ulong SentTsNs { get; }
}
