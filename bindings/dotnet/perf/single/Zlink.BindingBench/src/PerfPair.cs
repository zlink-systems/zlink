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

        // PERF_SINGLE_TEST_POLICY § 1.4 / C parity: the receiver waits on a
        // public poller, then performs a DontWait receive and drains the
        // currently available messages. The sender owns the active deadline
        // and sends the wire-level stop token from its own thread.
        using var poller = Zlink.CreatePoller();
        var events = new PollEvent[1];
        poller.Add(receiver, PollEventFlags.PollIn, 0);
        using var maybe = Received.Create();
        bool stopReceived = false;
        Exception? sendError = null;

        var senderThread = new Thread(() =>
        {
            try
            {
                long senderDeadlineTicks = DeadlineTicksFromSeconds(durationSeconds);
                ulong seq = 1;
                while (Stopwatch.GetTimestamp() < senderDeadlineTicks)
                {
                    StampMetricHeader(payload.AsSpan(), RunId, ActivePhase,
                        msgSize, seq, EpochNs());
                    seq++;
                    if (SendBlocking(sender, payload) <= 0)
                        continue;
                }
            }
            catch (Exception ex)
            {
                sendError = ex;
            }
            finally
            {
                if (!SendStopTokenBlocking(sender, "[single-pair]"))
                    sendError ??= new InvalidOperationException(
                        "pair stop token was not sent");
            }
        });
        senderThread.IsBackground = true;
        senderThread.Start();

        try
        {
            while (!stopReceived)
            {
                if (!WaitForInputSignalDriven(poller, events))
                    continue;

                while (TryReceiveNonBlocking(receiver, maybe))
                {
                    if (maybe.Parts.Count == 1
                        && StopToken.IsStopToken(maybe.FirstPart().AsReadOnlySpan()))
                    {
                        stopReceived = true;
                        break;
                    }
                    if (!PerfSocketIo.TryMeasurementPayload(maybe.Parts, out Message receivedPayload))
                        continue;
                    ReadOnlySpan<byte> body = receivedPayload.AsReadOnlySpan();

                    long recvTicks = Stopwatch.GetTimestamp();
                    if (TryDecodeExpectedSingleHeader(body, msgSize,
                            ActivePhase, out var header, RunId)
                        && recvTicks <= deadlineTicks)
                    {
                        received++;
                        ulong nowNs = EpochNs();
                        if (nowNs >= header.SentTsNs)
                        {
                            double latencyNs = nowNs - header.SentTsNs;
                            ReservoirSample(samples, latencyNs,
                                ref sampleSeen, latencyCap, ref rng);
                        }
                    }
                }
            }
        }
        catch (Exception ex)
        {
            recvError = ex;
        }

        senderThread.Join();

        latencySamples = samples;
        receivedOut = received;
        if (sendError != null || recvError != null)
            return false;

        return received > 0 && latencySamples.Count > 0;
    }
}
