using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfPubSub
{
    // PERF parity: C single PUBSUB publishes on topic "bench", subscribes
    // to the empty prefix (receive-all), and filters received messages by
    // topic. Matches bindings/c/perf/single/src/perf_pubsub.cpp.
    private const string Topic = "bench";
    private const uint RunId = 1;
    private const uint ActivePhase = 1;

    internal static int RunPubSub(string transport, int size)
    {
        int durationSeconds = ResolveSingleDurationSeconds();
        int recvTimeoutMs = ResolveSingleRcvTimeoutMs();
        int readyTimeoutMs = ResolveSingleConnectReadyTimeoutMs();
        int readySettleMs = ResolveSinglePubSubReadySettleMs();
        int latencySampleCap = ResolveSingleLatencyCount("PUBSUB");

        using var ctx = Zlink.CreateContext();
        ApplySingleContextOptions(ctx);
        using var pub = ctx.CreatePubSocket();
        using var sub = ctx.CreateSubSocket();
        ApplySingleSocketOptions(pub);
        ApplySingleSocketOptions(sub);
        RecalculateSingleAutoHwm(ctx);
        ConfigureTlsServerIfNeeded(pub, transport);
        ConfigureTlsClientIfNeeded(sub, transport);
        using var pubMonitor = pub.MonitorOpen(SocketEvent.ConnectionReady);
        using var subMonitor = sub.MonitorOpen(SocketEvent.ConnectionReady);

        try
        {
            string endpoint = EndpointFor(transport, "pubsub");
            int xpubNoDrop = PerfEnv.ReadPositive(
                "PERF_SINGLE_PUBSUB_XPUB_NODROP", 1) > 0 ? 1 : 0;
            pub.Options.NoDrop = xpubNoDrop != 0;
            pub.Bind(endpoint);
            endpoint = pub.Options.LastEndpoint;
            sub.SetSubscription(string.Empty);
            sub.Connect(endpoint);

            if (!(WaitForConnectionReady(pubMonitor, readyTimeoutMs)
                && WaitForConnectionReady(subMonitor, readyTimeoutMs)))
            {
                return 2;
            }

            Thread.Sleep(Math.Max(1, readySettleMs));

            int payloadSize = Math.Max(size, PerfMetricHeaderSize);
            var payload = new byte[payloadSize];
            Array.Fill(payload, (byte)'a');

            if (!RunActivePhase(pub, sub, payload, size, durationSeconds,
                    recvTimeoutMs, latencySampleCap, out long received,
                    out var latencySamples))
            {
                return 2;
            }

            // ITEM 1: byte-identical AUTO_HWM_DETAIL emission (C parity).
            EmitSingleAutoHwmDetail(pubMonitor, "PUBSUB", transport,
                "publisher", "pub", size);
            EmitSingleAutoHwmDetail(subMonitor, "PUBSUB", transport,
                "subscriber", "sub", size);

            double throughput = received / (double)Math.Max(durationSeconds, 1);
            var latency = ComputeLatencyStats(latencySamples);
            PrintResult("PUBSUB", transport, size, throughput, latency.mean,
                latency.p95, latency.p99);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"single_pubsub_error:{ex}");
            return 2;
        }
    }

    private static bool RunActivePhase(IPubSocket sender, ISubSocket receiver,
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
        // public poller, performs a DontWait subscribe, drains the available
        // burst, and exits only on the wire-level stop token.
        using var poller = Zlink.CreatePoller();
        var events = new PollEvent[1];
        poller.Add(receiver, PollEventFlags.PollIn, 0);
        using var maybe = new TopicMessage();
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
                    if (!PublishActiveMessageBlocking(sender, Topic, payload,
                            "[single-pubsub]"))
                        continue;
                }
            }
            catch (Exception ex)
            {
                sendError = ex;
            }
            finally
            {
                PublishStopTokenBlocking(sender, Topic, "[single-pubsub]");
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

                while (TrySubscribeNonBlocking(receiver, maybe))
                {
                    if (string.Equals(maybe.Topic, Topic,
                            StringComparison.Ordinal))
                    {
                        Message body = maybe.FirstPart();
                        ReadOnlySpan<byte> payloadSpan = body.AsReadOnlySpan();
                        if (StopToken.IsStopToken(payloadSpan))
                        {
                            stopReceived = true;
                            break;
                        }

                        long recvTicks = Stopwatch.GetTimestamp();
                        if (TryDecodeExpectedSingleHeader(payloadSpan,
                                msgSize, ActivePhase, out var header, RunId)
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

    private static bool TrySubscribeNonBlocking(ISubSocket receiver,
        TopicMessage result)
    {
        try
        {
            return receiver.Subscribe(result, RecvFlags.DontWait);
        }
        catch (ZlinkRecvException ex)
            when (ex.Result == ZlinkRecvException.ErrorCode.NoData
                  || IsInterrupted(ex.NativeErrno)
                  || IsWouldBlock(ex.NativeErrno))
        {
            return false;
        }
        catch (ZlinkException ex)
            when (IsInterrupted(ex.NativeErrno) || IsWouldBlock(ex.NativeErrno))
        {
            return false;
        }
    }

    private static bool IsInterrupted(int errno)
    {
        return PerfShared.IsInterrupted(errno);
    }
}
