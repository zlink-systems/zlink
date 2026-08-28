using System;
using System.Collections.Generic;
using System.Diagnostics;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiDealerDealerClient
{
    internal static async Task<int> Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int sndTimeoutMs = ResolveMultiSndTimeoutMs(options);
        int rcvTimeoutMs = ResolveMultiRcvTimeoutMs(options);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        ulong monitorHwmBytes = ResolveMultiMonitorHwmBytes();
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
                var monitor = client.MonitorOpen(SocketEvent.ConnectionReady,
                    monitorHwmBytes);
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

            if (!await RunSendPhaseAsync(activeClients, size, durationSeconds,
                    controlState).ConfigureAwait(false))
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

    private static async Task<bool> RunSendPhaseAsync(
        List<ISocket> activeClients, int msgSize,
        int durationSeconds, RunnerControlState controlState)
    {
        const uint runId = 1;
        long seq = 0;
        long sent = 0;
        int payloadSize = Math.Max(msgSize, PerfMetricHeaderSize);
        long activeDeadlineTicks = Stopwatch.GetTimestamp()
            + (long)Math.Max(1, durationSeconds) * Stopwatch.Frequency;

        async Task SendLoopAsync(IDealerSocket socket)
        {
            while (!controlState.StopRequested
                   && Stopwatch.GetTimestamp() < activeDeadlineTicks)
            {
                using Message message = Message.Allocate(payloadSize);
                ulong currentSeq = unchecked((ulong)Interlocked.Increment(ref seq));
                StampMetricHeader(message.AsSpan(), runId,
                    PerfPhase.Active, msgSize, currentSeq, EpochNs());
                await PerfSocketIo.SendMeasurementAsync(socket, message,
                    SendFlags.None).ConfigureAwait(false);
                Interlocked.Increment(ref sent);
            }
        }

        var sendTasks = new Task[activeClients.Count];
        for (int i = 0; i < activeClients.Count; i++)
            sendTasks[i] = SendLoopAsync((IDealerSocket)activeClients[i]);
        await Task.WhenAll(sendTasks).ConfigureAwait(false);

        // PERF_MULTI_TEST_POLICY § 1.3.1 / C
        // perf_multi_dealer_dealer_client.cpp sends one blocking wire-level
        // stop token on every client socket after the active window. The
        // server drains these tokens before the size process closes.
        for (int i = 0; i < activeClients.Count; i++)
        {
            if (!SendStopTokenBlocking((IDealerSocket)activeClients[i],
                    controlState))
                return false;
        }

        return Volatile.Read(ref sent) > 0;
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
                socket.Send().Message(message).Submit(SendFlags.None);
                return true;
            }
            catch (ZlinkSubmitException ex)
                when (ex.Result == ZlinkSubmitException.ErrorCode.Backpressured
                      || ex.Result == ZlinkSubmitException.ErrorCode.NotAdmitted
                      || IsWouldBlock(ex.NativeErrno)
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

}
