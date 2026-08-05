using System;
using System.Collections.Generic;
using System.Diagnostics;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiDealerDealerClient
{
    private const int ActiveDeadlineTag = int.MaxValue;

    internal static int Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int sndTimeoutMs = ResolveMultiSndTimeoutMs(options);
        int rcvTimeoutMs = ResolveMultiRcvTimeoutMs(options);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
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
                var client = ctx.CreateDealerSocket();
                ApplyMultiSocketOptions(client, options);
                ConfigureTlsClientIfNeeded(client, options.Transport);
                client.Options.SendTimeout = TimeSpan.FromMilliseconds(sndTimeoutMs);
                client.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeoutMs);
                client.SetRoutingId(RoutingId.From(
                    System.Text.Encoding.ASCII.GetBytes($"client_{i}")));
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

            if (!RunSendPhase(activeClients, size, durationSeconds,
                    controlState))
                return 2;

            WriteStdoutLine($"CLIENT_DONE,{size}");
            return 0;
        }
        finally
        {
            DisposeAllQuietly(monitors);
            DisposeAllQuietly(clients);
        }
    }

    private static bool RunSendPhase(List<ISocket> activeClients, int msgSize,
        int durationSeconds, RunnerControlState controlState)
    {
        const uint runId = 1;
        ulong seq = 1;
        int payloadSize = Math.Max(msgSize, PerfMetricHeaderSize);
        long activeDeadlineTicks = Stopwatch.GetTimestamp()
            + (long)Math.Max(1, durationSeconds) * Stopwatch.Frequency;
        using var activeTimer = Zlink.CreateTimer();
        using var sendPoller = Zlink.CreatePoller();
        var sendEvents = new PollEvent[activeClients.Count + 1];
        var pending = new bool[activeClients.Count];
        int pendingCount = 0;

        for (int i = 0; i < activeClients.Count; i++)
            sendPoller.Add(activeClients[i], PollEventFlags.PollOut, (nuint)i);
        sendPoller.Add(activeTimer, ActiveDeadlineTag);
        activeTimer.Start(TimeSpan.FromSeconds(Math.Max(1, durationSeconds)),
            1);

        while (!controlState.StopRequested
               && Stopwatch.GetTimestamp() < activeDeadlineTicks)
        {
            bool progressed = false;
            for (int i = 0; i < activeClients.Count; i++)
            {
                if (pending[i])
                    continue;

                IDealerSocket socket = (IDealerSocket)activeClients[i];
                while (!controlState.StopRequested
                       && Stopwatch.GetTimestamp() < activeDeadlineTicks)
                {
                    Message message = Message.Allocate(payloadSize);
                    StampMetricHeader(message.AsSpan(), runId,
                        PerfPhase.Active, msgSize, seq, EpochNs());
                    bool sent = socket.Send().Message(message)
                        .Flags(SendFlags.DontWait).Submit();
                    message.Dispose();
                    if (!sent)
                    {
                        pending[i] = true;
                        pendingCount++;
                        break;
                    }

                    seq++;
                    progressed = true;
                }
            }

            if (!progressed && pendingCount > 0
                && Stopwatch.GetTimestamp() < activeDeadlineTicks)
            {
                if (!WaitForWritable(sendPoller, sendEvents, activeTimer, pending,
                        ref pendingCount))
                    break;
            }
        }

        // PERF_MULTI_TEST_POLICY § 1.3.1 / C
        // perf_multi_dealer_dealer_client.cpp run_send_phase (~290-293): the
        // one-way sender blocking-sends a wire-level stop token on EVERY
        // client socket after the send window so the dealer-dealer server's
        // signal-driven receiver loop terminates. Send failure is fatal.
        for (int i = 0; i < activeClients.Count; i++)
        {
            if (!SendStopTokenBlocking((IDealerSocket)activeClients[i],
                    controlState))
                return false;
        }

        return true;
    }

    // Mirrors C send_stop_token(): blocking send with retry through
    // transient backpressure (EINTR/EAGAIN/EWOULDBLOCK/ETIMEDOUT), aborting
    // the retry loop early only when shutdown is requested
    // (C g_stop_requested). Any non-transient failure is fatal.
    private static bool SendStopTokenBlocking(IDealerSocket socket,
        RunnerControlState controlState)
    {
        while (!controlState.StopRequested)
        {
            try
            {
                using Message message = new(MultiStopToken.AsSpan());
                if (socket.Send().Message(message).Flags(SendFlags.None)
                        .Submit())
                    return true;
            }
            catch (ZlinkException ex)
                when (IsWouldBlock(ex.NativeErrno)
                      || IsInterrupted(ex.NativeErrno)
                      || PerfShared.IsTransientBackpressure(ex.NativeErrno)
                      || PerfShared.IsTransientNetworkError(ex.NativeErrno))
            {
                continue;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"[multi-dealer-dealer-client] stop-token send failed: {ex.Message}");
                return false;
            }
        }

        return true;
    }

    private static bool WaitForWritable(IPoller poller, PollEvent[] events,
        Systems.Zlink.IZlinkTimer activeTimer, bool[] pending, ref int pendingCount)
    {
        int written = poller.Wait(events,
            TimeSpan.FromMilliseconds(MultiClientPollTimeoutMs));
        for (int i = 0; i < written; i++)
        {
            if (events[i].Slot == (nuint)ActiveDeadlineTag)
            {
                _ = activeTimer.Recv();
                return false;
            }

            nuint slot = events[i].Slot;
            if ((events[i].Revents & PollEventFlags.PollOut) == 0
                || slot > (nuint)int.MaxValue
                || (int)slot >= pending.Length
                || !pending[(int)slot])
                continue;

            int index = (int)slot;
            pending[index] = false;
            pendingCount--;
        }

        return true;
    }
}
