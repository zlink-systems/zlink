using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfPair
{
    private const uint RunId = 1;
    private const uint ActivePhase = 1;

    internal static int RunPair(string transport, int size)
    {
        int durationSeconds = ResolveSingleDurationSeconds();
        int recvTimeoutMs = ResolveSingleRcvTimeoutMs();
        int readyTimeoutMs = ResolveSingleConnectReadyTimeoutMs();
        int latencySampleCap = ResolveSingleLatencyCount("PAIR");

        using var ctx = Zlink.CreateContext();
        ApplySingleContextOptions(ctx);
        using var left = ctx.CreatePairSocket();
        using var right = ctx.CreatePairSocket();
        ApplySingleSocketOptions(left);
        ApplySingleSocketOptions(right);
        ApplySingleAutoHwmMsgUnit(ctx, size);
        RecalculateSingleAutoHwm(ctx);
        ConfigureTlsServerIfNeeded(left, transport);
        ConfigureTlsClientIfNeeded(right, transport);
        using var leftMonitor = left.MonitorOpen(SocketEvent.ConnectionReady);
        using var rightMonitor = right.MonitorOpen(SocketEvent.ConnectionReady);

        try
        {
            string endpoint = EndpointFor(transport, "pair");
            left.Bind(endpoint);
            endpoint = left.Options.LastEndpoint;
            right.Connect(endpoint);
            if (!(WaitForConnectionReady(leftMonitor, readyTimeoutMs)
                && WaitForConnectionReady(rightMonitor, readyTimeoutMs)))
            {
                return 2;
            }

            int payloadSize = Math.Max(size, PerfMetricHeaderSize);
            var payload = new byte[payloadSize];
            Array.Fill(payload, (byte)'a');

            if (!RunActivePhase(right, left, payload, size, durationSeconds,
                    recvTimeoutMs, latencySampleCap, out long received,
                    out var latencySamples))
            {
                Console.Error.WriteLine(
                    $"single_pair_active_failed:received={received},samples={latencySamples.Count}");
                return 2;
            }

            // ITEM 1: emit AUTO_HWM_DETAIL for both sockets (receiver=bind,
            // sender=connect) so run_emit.py renders the "## Auto-HWM Detail"
            // block byte-identically to the C single report.
            EmitSingleAutoHwmDetail(leftMonitor, "PAIR", transport,
                "receiver", "pair", size);
            EmitSingleAutoHwmDetail(rightMonitor, "PAIR", transport,
                "sender", "pair", size);

            double throughput = received / (double)Math.Max(durationSeconds, 1);
            var latency = ComputeLatencyStats(latencySamples);
            PrintResult("PAIR", transport, size, throughput, latency.mean,
                latency.p95, latency.p99);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"single_pair_error:{ex}");
            return 2;
        }
    }

    private static bool RunActivePhase(IPairSocket sender, IPairSocket receiver,
        byte[] payload, int msgSize, int durationSeconds, int recvTimeoutMs,
        int latencyCap, out long receivedOut, out List<double> latencySamples)
    {
        _ = recvTimeoutMs;
        long deadlineTicks = DeadlineTicksFromSeconds(durationSeconds);

        long received = 0;
        Exception? recvError = null;
        var samples = new List<double>(Math.Max(0, latencyCap));
        long sampleSeen = 0;
        uint rng = 0xA341316Cu;

        // PERF_SINGLE_TEST_POLICY § 1.4 / C parity: the C reference receiver
        // (bindings/c/perf/single/common/perf_single_one_way.hpp
        // run_active_phase) does a *blocking* recv (zlink_recv_part flags=0)
        // for the first message of each cycle, then burst-drains with
        // DontWait until EAGAIN, and exits on the wire-level stop token. We
        // mirror that exactly here (no IPoller / no DontWait spin loop).
        var recvThread = new Thread(() =>
        {
            // Reuse one Received envelope for the whole phase (parity with C
            // which reuses a single stack header buffer). Allocating a fresh
            // envelope per message churned the managed heap on the hot
            // receiver thread.
            using var maybe = Received.Create();

            try
            {
                while (true)
                {
                    try
                    {
                        if (!receiver.Recv(maybe, RecvFlags.None))
                            continue;
                    }
                    catch (ZlinkException ex)
                        when (IsInterrupted(ex.NativeErrno)
                              || IsWouldBlock(ex.NativeErrno))
                    {
                        // RCVTIMEO expiry on an idle socket: C's blocking
                        // recv returns EAGAIN and re-loops; mirror that.
                        continue;
                    }

                    bool drain = true;
                    while (drain)
                    {
                        {
                            ReadOnlySpan<byte> body = maybe.FirstPart()
                                .AsReadOnlySpan();
                            if (StopToken.IsStopToken(body))
                                return;

                            long recvTicks = Stopwatch.GetTimestamp();
                            if (TryDecodeExpectedSingleHeader(body, msgSize,
                                    ActivePhase, out var header, RunId)
                                && recvTicks <= deadlineTicks)
                            {
                                Interlocked.Increment(ref received);
                                ulong nowNs = EpochNs();
                                if (nowNs >= header.SentTsNs)
                                {
                                    double latencyNs = nowNs - header.SentTsNs;
                                    ReservoirSample(samples, latencyNs,
                                        ref sampleSeen, latencyCap, ref rng);
                                }
                            }
                        }

                        drain = receiver.Recv(maybe, RecvFlags.DontWait);
                    }
                }
            }
            catch (Exception ex)
            {
                recvError = ex;
            }
        });
        recvThread.IsBackground = true;
        recvThread.Start();

        bool sendFailed = false;
        ulong seq = 1;
        while (Stopwatch.GetTimestamp() < deadlineTicks)
        {
            StampMetricHeader(payload.AsSpan(), RunId, ActivePhase, msgSize, seq,
                EpochNs());
            seq++;
            try
            {
                if (SendBlocking(sender, payload) <= 0)
                    continue;
            }
            catch (ZlinkException ex)
                when (PerfShared.IsTransientBackpressure(ex.NativeErrno))
            {
                continue;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[single-pair] send failed: {ex.Message}");
                sendFailed = true;
                break;
            }
        }

        // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end via wire-level
        // stop token. Bounded retry through transient backpressure so the
        // receiver always observes the terminator.
        SendStopTokenBlocking(sender, "[single-pair]");
        recvThread.Join();

        latencySamples = samples;
        receivedOut = received;
        if (sendFailed || recvError != null)
            return false;

        return received > 0 && latencySamples.Count > 0;
    }
}
