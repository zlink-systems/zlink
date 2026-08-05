using System;
using Systems.Zlink;

internal static partial class PerfRunner
{
    internal static int SendBlocking(IMessageSocket socket,
        ReadOnlySpan<byte> buffer,
        SendFlags flags = SendFlags.None)
    {
        return PerfSocketIo.Send(socket, buffer, flags);
    }

    internal static bool IsEchoPattern(string pattern)
    {
        string normalized = NormalizePerfPattern(pattern);
        return normalized == "DEALER_ROUTER"
            || normalized == "DEALER_ROUTER_REQREP"
            || normalized == "ROUTER_ROUTER"
            || normalized == "ROUTER_ROUTER_REQREP"
            || normalized == "STREAM";
    }

    internal static void PrintResult(string pattern, string transport, int size,
        double throughput, double latencyNs)
    {
        PrintResult(pattern, transport, size, throughput, latencyNs, latencyNs,
            latencyNs);
    }

    internal static void PrintResult(string pattern, string transport, int size,
        double throughput, double latencyNs, double latencyP95Ns,
        double latencyP99Ns)
    {
        PerfShared.PrintResult(pattern, transport, size, throughput, latencyNs,
            latencyP95Ns, latencyP99Ns, BandwidthMultiplier(pattern),
            fixedFormat: true);
    }

    private static double BandwidthMultiplier(string pattern)
    {
        return IsEchoPattern(pattern) ? 2.0 : 1.0;
    }

    internal static bool StampMetricHeader(Span<byte> payload, uint runId,
        PerfPhase phase, int msgSize, ulong seq, ulong sentTsNs)
    {
        return PerfShared.StampMetricHeader(payload, runId, (uint)phase,
            msgSize, seq, sentTsNs);
    }

    internal static bool TryDecodeMetricHeader(ReadOnlySpan<byte> payload,
        out PerfMetricHeader header)
    {
        return PerfShared.TryDecodeMetricHeader(payload, out header);
    }

    internal static bool IsTransientNetworkError(int errno)
    {
        return PerfShared.IsTransientNetworkError(errno);
    }

}
