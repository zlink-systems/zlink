using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiDealerDealerServer
{
    private const int ServerSocketTag = 0;

    private enum ReceiveStatus
    {
        NoData,
        Message,
        StopToken,
    }

    internal static int Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        int clientCount = ResolveMultiClients(options);
        int durationSeconds = ResolveMultiDurationSeconds(options);
        int latencySampleCap = ResolveMultiLatencySampleCap(options);
        string endpoint = MultiEndpointFor(options.Transport,
            "multi-dealer-dealer", options);

        using var ctx = Zlink.CreateContext();
        using var pollManager = new PollManager();
        using var controlState = new RunnerControlState(size);
        ApplyMultiServerContextOptions(ctx, options);
        using var server = ctx.CreateDealerSocket();
        ApplyMultiSocketOptions(server, options);
        ConfigureTlsServerIfNeeded(server, options.Transport);
        using var monitor = server.MonitorOpen(SocketEvent.ConnectionReady);

        server.Bind(endpoint);
        endpoint = server.Options.LastEndpoint;
        WriteStdoutLine($"READY,{endpoint}");

        if (!WaitConnectReadyCount(monitor, clientCount, readyTimeoutMs))
            return 2;

        ApplyAutoHwmMsgUnit(ctx, size);
        RecalculateAutoHwm(ctx);
        PrintAutoHwmSnapshot(server, "server", options.Transport, size);

        if (!controlState.WaitForStart(readyTimeoutMs))
        {
            if (!controlState.StopRequested)
                Console.Error.WriteLine("multi_server_error:no_start");
            return controlState.StopRequested ? 0 : 2;
        }

        var result = RunReceivePhase(server, size, latencySampleCap,
            durationSeconds);
        if (result.measureCount <= 0)
            return 2;

        PrintResult(options.Pattern, options.Transport, size, result.throughput,
            result.latencyNs, result.latencyP95Ns, result.latencyP99Ns);
        return 0;
    }

    private static (double throughput, double latencyNs, double latencyP95Ns,
        double latencyP99Ns, long measureCount)
        RunReceivePhase(IDealerSocket server, int msgSize, int latencySampleCap,
            int durationSeconds)
    {
        const uint expectedRunId = 1;
        var latSamples = new LatencySampleBuffer(
            EstimateLatencySampleCapacity(latencySampleCap, durationSeconds));
        long measureCount = 0;
        using var received = Received.Create();

        // PERF_MULTI_TEST_POLICY: the active window ends purely on the
        // configured duration (signal-driven -1 poll, duration timer), like C
        // perf_multi_dealer_dealer_server.cpp run_receive_window. C's
        // subsequent drain_phase_until_idle exists ONLY because the C multi
        // server process is reused across every size case; the .NET multi
        // runner (multi/run_benchmarks.sh) spawns and tears down a fresh
        // server process per size (for size loop, server start + STOP/wait
        // each iteration), so there is no cross-size backlog to drain. The
        // process exits after this size, discarding any tail like C discards
        // it across runs. No idle-drain phase is performed.
        if (!ReceiveActiveWindow(server, received, msgSize, expectedRunId,
                PerfPhase.Active, latSamples, ref measureCount,
                durationSeconds))
        {
            return (0.0, 0.0, 0.0, 0.0, 0);
        }

        double configuredSeconds = Math.Max(1.0, durationSeconds);
        double throughput = measureCount / configuredSeconds;
        // PERF_POLICY: report measured latency only. C
        // normalize_latency_stats reports zeros when no samples and never
        // fabricates a duration-derived latency.
        var latency = latSamples.ComputeStats();
        double latencyNs = latency.mean;
        double latencyP95Ns = Math.Max(latency.p95, latencyNs);
        double latencyP99Ns = Math.Max(latency.p99, latencyP95Ns);

        return (throughput, latencyNs, latencyP95Ns, latencyP99Ns, measureCount);
    }

    private static int EstimateLatencySampleCapacity(int configuredCap,
        int durationSeconds)
    {
        const int expectedHighRatePerSecond = 4_000_000;
        long expected = (long)Math.Max(1, durationSeconds)
            * expectedHighRatePerSecond;
        if (expected > int.MaxValue)
            expected = int.MaxValue;
        return Math.Max(configuredCap, (int)expected);
    }

    private static bool ReceiveActiveWindow(IDealerSocket server,
        Received received, int msgSize, uint expectedRunId,
        PerfPhase expectedPhase, LatencySampleBuffer latSamples,
        ref long messageCount, int durationSeconds)
    {
        using var poller = Zlink.CreatePoller();
        var events = new PollEvent[1];
        long activeDeadlineTicks = Stopwatch.GetTimestamp()
            + (long)Math.Max(1, durationSeconds) * Stopwatch.Frequency;
        poller.Add(server, PollEventFlags.PollIn, ServerSocketTag);

        while (true)
        {
            int written = poller.Wait(events,
                TimeSpan.FromMilliseconds(-1));

            for (int i = 0; i < written; i++)
            {
                if (events[i].Slot != (nuint)ServerSocketTag
                    || (events[i].Revents & PollEventFlags.PollIn) == 0)
                    continue;

                ReceiveStatus receiveStatus = ReceiveOneAvailable(server,
                    received, msgSize, expectedRunId, expectedPhase,
                    latSamples, ref messageCount, collectMetrics: true);
                if (receiveStatus == ReceiveStatus.NoData)
                    continue;

                // The sender's stop token is the wire-level phase boundary.
                // Treating it as an ordinary payload can leave a signal-driven
                // poll blocked forever when the token arrives just before the
                // local deadline.
                if (receiveStatus == ReceiveStatus.StopToken)
                    return true;

                if (Stopwatch.GetTimestamp() >= activeDeadlineTicks)
                    return true;

                if (DrainAvailable(server, received, msgSize, expectedRunId,
                        expectedPhase, latSamples, ref messageCount,
                        collectMetrics: true))
                    return true;

                if (Stopwatch.GetTimestamp() >= activeDeadlineTicks)
                    return true;
            }
        }
    }

    private static ReceiveStatus ReceiveOneAvailable(IDealerSocket server,
        Received received, int msgSize, uint expectedRunId,
        PerfPhase expectedPhase, LatencySampleBuffer latSamples,
        ref long messageCount, bool collectMetrics)
    {
        if (!server.Recv(received, RecvFlags.DontWait))
            return ReceiveStatus.NoData;

        bool stopToken = ProcessReceived(server, received, msgSize,
            expectedRunId, expectedPhase, latSamples, ref messageCount,
            collectMetrics);
        return stopToken ? ReceiveStatus.StopToken : ReceiveStatus.Message;
    }

    private static bool DrainAvailable(IDealerSocket server, Received received,
        int msgSize, uint expectedRunId, PerfPhase expectedPhase,
        LatencySampleBuffer latSamples, ref long messageCount,
        bool collectMetrics)
    {
        while (true)
        {
            ReceiveStatus receiveStatus = ReceiveOneAvailable(server,
                received, msgSize, expectedRunId, expectedPhase,
                latSamples, ref messageCount,
                collectMetrics);
            if (receiveStatus == ReceiveStatus.NoData)
                return false;
            if (receiveStatus == ReceiveStatus.StopToken)
                return true;
        }
    }

    private static bool ProcessReceived(IDealerSocket server,
        Received received, int msgSize, uint expectedRunId,
        PerfPhase expectedPhase, LatencySampleBuffer latSamples,
        ref long messageCount, bool collectMetrics)
    {
        if (!received.IsSinglePart)
            return false;

        ReadOnlySpan<byte> body = received.FirstPart().AsReadOnlySpan();

        if (IsStopTokenPayload(body))
            return true;

        if (!TryDecodeActiveHeader(body, msgSize, expectedRunId,
                expectedPhase, out ulong sentTsNs))
            return false;

        if (!collectMetrics)
            return false;

        messageCount++;
        if (sentTsNs > 0)
        {
            ulong nowNs = EpochNs();
            if (nowNs >= sentTsNs)
                latSamples.Add(nowNs - sentTsNs);
        }

        return false;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static bool TryDecodeActiveHeader(ReadOnlySpan<byte> payload,
        int expectedMsgSize, uint expectedRunId, PerfPhase expectedPhase,
        out ulong sentTsNs)
    {
        sentTsNs = 0;
        if (payload.Length < PerfMetricHeaderSize)
            return false;

        ref byte head = ref MemoryMarshal.GetReference(payload);
        if (Unsafe.ReadUnaligned<uint>(ref head) != PerfShared.PerfMetricMagic)
            return false;
        if (Unsafe.ReadUnaligned<uint>(ref Unsafe.Add(ref head, 4))
            != expectedRunId)
            return false;
        if (Unsafe.Add(ref head, 8) != (byte)expectedPhase)
            return false;
        if (Unsafe.ReadUnaligned<uint>(ref Unsafe.Add(ref head, 9))
            != (uint)expectedMsgSize)
            return false;

        sentTsNs = (ulong)Unsafe.ReadUnaligned<long>(
            ref Unsafe.Add(ref head, 21));
        return true;
    }

    private sealed class LatencySampleBuffer
    {
        private double[] _samples;
        private int _count;
        private double _sum;

        internal LatencySampleBuffer(int capacity)
        {
            _samples = new double[Math.Max(1, capacity)];
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal void Add(double value)
        {
            if (_count == _samples.Length)
                Grow();
            double sample = value >= 0.0 ? value : 0.0;
            _samples[_count++] = sample;
            _sum += sample;
        }

        internal (double mean, double p95, double p99) ComputeStats()
        {
            if (_count == 0)
                return (0.0, 0.0, 0.0);

            Array.Sort(_samples, 0, _count);
            double mean = _sum / _count;
            return (mean, PercentileFromSorted(0.95),
                PercentileFromSorted(0.99));
        }

        private void Grow()
        {
            int next = _samples.Length <= int.MaxValue / 2
                ? _samples.Length * 2
                : int.MaxValue;
            if (next == _samples.Length)
                throw new OutOfMemoryException("Latency sample buffer is full.");
            Array.Resize(ref _samples, next);
        }

        private double PercentileFromSorted(double q)
        {
            if (_count == 0)
                return 0.0;
            if (q <= 0.0)
                return _samples[0];
            if (q >= 1.0)
                return _samples[_count - 1];

            double pos = (_count - 1) * q;
            int lo = (int)pos;
            int hi = lo + 1 < _count ? lo + 1 : lo;
            double frac = pos - lo;
            return _samples[lo] + (_samples[hi] - _samples[lo]) * frac;
        }
    }
}
