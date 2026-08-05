using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiPubSubClient
{
    internal static int Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int sndTimeoutMs = ResolveMultiSndTimeoutMs(options);
        int rcvTimeoutMs = ResolveMultiRcvTimeoutMs(options);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        int latencySampleCap = ResolveMultiLatencySampleCap(options);
        int clientCount = ResolveMultiClients(options);
        int durationSeconds = ResolveMultiDurationSeconds(options);
        string endpoint = options.Endpoint;

        using var ctx = Zlink.CreateContext();
        using var pollManager = new PollManager();
        using var controlState = new RunnerControlState(size);
        ApplyMultiClientContextOptions(ctx, options);
        var clients = new List<ISocket>(clientCount);
        var monitors = new List<MonitorSocket>(clientCount);
        try
        {
            for (int i = 0; i < clientCount; i++)
            {
                var client = ctx.CreateSubSocket();
                ApplyMultiSocketOptions(client, options);
                ConfigureTlsClientIfNeeded(client, options.Transport);
                client.Options.SendTimeout = TimeSpan.FromMilliseconds(sndTimeoutMs);
                client.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeoutMs);
                client.SetSubscription(string.Empty);
                var monitor = client.MonitorOpen(SocketEvent.ConnectionReady);
                client.Connect(endpoint);
                clients.Add(client);
                monitors.Add(monitor);
            }

            List<ISocket> activeClients = WaitClientConnectReadyAll(
                pollManager, clients, monitors, readyTimeoutMs);
            if (activeClients.Count != clients.Count)
            {
                Console.Error.WriteLine("multi_client_error:no_ready_connections");
                return 2;
            }
            DisposeAllQuietly(monitors);
            monitors.Clear();

            for (int i = 0; i < clients.Count; i++)
                ApplyAutoHwmMsgUnit(ctx, size);
            RecalculateAutoHwm(ctx);
            if (clients.Count > 0)
                PrintAutoHwmSnapshot(clients[0], "endpoint",
                    options.Transport, size);

            WriteStdoutLine($"CLIENT_READY,{size}");

            if (!controlState.WaitForStart(readyTimeoutMs))
            {
                if (!controlState.StopRequested)
                    Console.Error.WriteLine("multi_client_error:no_start");
                return controlState.StopRequested ? 0 : 2;
            }

            var result = RunMultiPubSubClientLoop(pollManager, activeClients,
                size, latencySampleCap, durationSeconds,
                ResolveMultiClientPollTimeoutMs(options));

            if (result.measureCount <= 0)
                return 2;

            PrintResult(options.Pattern, options.Transport, size, result.throughput,
                result.latencyNs, result.latencyP95Ns, result.latencyP99Ns);
            WriteStdoutLine($"CLIENT_DONE,{size}");
            return 0;
        }
        finally
        {
            DisposeAllQuietly(monitors);
            DisposeAllQuietly(clients);
        }
    }

    private static (double throughput, double latencyNs, double latencyP95Ns,
        double latencyP99Ns, long measureCount)
        RunMultiPubSubClientLoop(PollManager pollManager,
            List<ISocket> activeClients, int msgSize, int latencySampleCap,
            int durationSeconds, int pollTimeoutMs)
    {
        _ = pollManager;
        // PERF_MULTI_TEST_POLICY § 1.3.1: poll timeout is unconditionally -1
        // (signal-driven). The caller resolves it to -1; assert that here so
        // the wait below is never a timer loop.
        _ = pollTimeoutMs;
        const uint expectedRunId = 1;
        var activeLatSamples = new List<double>(latencySampleCap);
        long activeSampleSeen = 0;
        uint rng = 0xA341316Cu;
        long measureCount = 0;

        // C perf_multi_pubsub_client.cpp run_recv_duration: active_deadline
        // only gates whether a received sample is *recorded*; the loop ends
        // purely on the wire stop token or a cooldown-phase header. There is
        // no separate stop deadline / timer cap (PERF_MULTI § 1.3.1).
        long benchDeadlineTicks = Stopwatch.GetTimestamp()
            + (long)Math.Max(1, durationSeconds) * Stopwatch.Frequency;
        using var poller = Zlink.CreatePoller();
        var events = new PollEvent[activeClients.Count];
        var subscribedMessages = new TopicMessage[activeClients.Count];
        for (int i = 0; i < activeClients.Count; i++)
        {
            poller.Add(activeClients[i], PollEventFlags.PollIn, (nuint)i);
            subscribedMessages[i] = new TopicMessage();
        }

        try
        {
            bool phaseDone = false;
            while (!phaseDone)
            {
                int readyCount = WaitForReadReady(poller, events);
                if (readyCount <= 0)
                    continue;

                for (int readyOffset = 0; readyOffset < readyCount; readyOffset++)
                {
                    nuint slot = events[readyOffset].Slot;
                    if (slot > (nuint)int.MaxValue
                        || (int)slot >= activeClients.Count
                        || (events[readyOffset].Revents & PollEventFlags.PollIn) == 0)
                    {
                        continue;
                    }

                    int i = (int)slot;
                    TopicMessage subscribed = subscribedMessages[i];
                    while (true)
                    {
                        if (!TrySubscribeNoWait((ISubSocket)activeClients[i],
                                subscribed))
                            break;

                        ReadOnlySpan<byte> body = subscribed.FirstPart()
                            .AsReadOnlySpan();
                        if (IsStopTokenPayload(body))
                        {
                            phaseDone = true;
                            break;
                        }

                        long recvTicks = Stopwatch.GetTimestamp();
                        if (recvTicks > benchDeadlineTicks)
                            continue;

                        bool headerOk = PerfRunner.TryDecodeMetricHeader(body,
                            out PerfMetricHeader header);
                        if (!headerOk
                            || header.RunId != expectedRunId
                            || header.MsgSize != (uint)msgSize)
                        {
                            continue;
                        }

                        if (header.Phase == (uint)PerfPhase.Active)
                        {
                            measureCount++;
                            if (header.SentTsNs > 0)
                            {
                                AddLatencySample(activeLatSamples,
                                    ref activeSampleSeen, latencySampleCap,
                                    ref rng, header);
                            }
                        }
                        else if (header.Phase == (uint)PerfPhase.Cooldown)
                        {
                            phaseDone = true;
                            break;
                        }
                    }
                }
            }
        }
        finally
        {
            for (int i = 0; i < subscribedMessages.Length; i++)
                subscribedMessages[i]?.Dispose();
        }

        double configuredSeconds = Math.Max(1.0, durationSeconds);
        double throughput = measureCount / configuredSeconds;
        // PERF_POLICY: report measured latency only. C
        // normalize_latency_stats reports zeros when no samples and never
        // fabricates a duration-derived latency.
        var latency = ComputeLatencyStats(activeLatSamples);
        double latencyNs = latency.mean;
        double latencyP95Ns = Math.Max(latency.p95, latencyNs);
        double latencyP99Ns = Math.Max(latency.p99, latencyP95Ns);

        return (throughput, latencyNs, latencyP95Ns, latencyP99Ns, measureCount);
    }

    // PERF_MULTI_TEST_POLICY § 1.3.1: signal-driven (-1) poller wait, woken
    // by the wire stop token published over the same topic. Matches C
    // perf_multi_pubsub_client.cpp run_recv_duration's
    // zlink_poller_wait(...,-1,NULL). No timer fallback / no stop deadline.
    private static int WaitForReadReady(IPoller poller, PollEvent[] events)
    {
        try
        {
            return poller.Wait(events, Timeout.InfiniteTimeSpan);
        }
        catch (ZlinkException ex) when (IsWouldBlock(ex.NativeErrno)
                                        || IsInterrupted(ex.NativeErrno))
        {
            return 0;
        }
    }

    private static void AddLatencySample(List<double> samples,
        ref long sampleSeen, int latencySampleCap, ref uint rng,
        PerfMetricHeader header)
    {
        if (header.SentTsNs == 0)
            return;
        ulong nowNs = EpochNs();
        if (nowNs < header.SentTsNs)
            return;
        ReservoirSample(samples, nowNs - header.SentTsNs, ref sampleSeen,
            latencySampleCap, ref rng);
    }

    private static bool TrySubscribeNoWait(ISubSocket socket,
        TopicMessage result)
    {
        try
        {
            return socket.Subscribe(result, RecvFlags.DontWait);
        }
        catch (ZlinkException ex) when (IsWouldBlock(ex.NativeErrno)
                                        || IsInterrupted(ex.NativeErrno))
        {
            return false;
        }
    }
}
