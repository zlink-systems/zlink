using System;
using System.Collections.Generic;
using System.Diagnostics;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiDealerDealerClient
{
    internal static int Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int rcvTimeoutMs = ResolveMultiRcvTimeoutMs(options);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        ulong monitorHwmBytes = ResolveMultiMonitorHwmBytes();
        int clientCount = ResolveMultiClients(options);
        int durationSeconds = ResolveMultiDurationSeconds(options);
        int latencyDurationSeconds = ResolveMultiLatencyDurationSeconds(options);
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

            if (!RunSendPhase(pollManager, activeClients, size,
                    durationSeconds, latencyDurationSeconds, readyTimeoutMs,
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

    private static bool RunSendPhase(PollManager pollManager, List<ISocket> activeClients, int msgSize,
        int durationSeconds, int latencyDurationSeconds, int readyTimeoutMs,
        RunnerControlState controlState)
    {
        long seq = 0;
        _ = pollManager.PollSockets(activeClients,
            PollEventFlags.PollCompletion, 0);
        ulong activeEndNs = EpochNs() + (ulong)durationSeconds * 1_000_000_000UL;
        Func<bool> stopRequested = () => controlState.StopRequested;
        long sent = RunSendWindow(pollManager, activeClients, msgSize,
            latencyDurationSeconds, null, activeEndNs, stopRequested, ref seq);

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

        if (sent <= 0
            || !controlState.WaitForPhase("PHASE_PACED", readyTimeoutMs,
                out long activeReceived)
            || activeReceived <= 0)
            return false;

        var schedule = new MultiPacedSchedule(
            MultiTargetRate(activeReceived, durationSeconds));
        RunSendWindow(pollManager, activeClients, msgSize,
            latencyDurationSeconds, schedule, schedule.EndNs(latencyDurationSeconds),
            stopRequested, ref seq);
        for (int i = 0; i < activeClients.Count; i++)
        {
            if (!SendStopTokenBlocking((IDealerSocket)activeClients[i],
                    controlState))
                return false;
        }

        if (!controlState.WaitForPhase("PHASE_COMPLETE", readyTimeoutMs,
                out long pacedReceived))
            return false;
        PrintMultiBenchDiag("DEALER_DEALER", msgSize, sent,
            durationSeconds, schedule, activeReceived,
            pacedReceived / (double)latencyDurationSeconds);
        return true;
    }

    internal static long RunSendWindow(PollManager pollManager,
        List<ISocket> sockets, int msgSize, int latencyDurationSeconds,
        MultiPacedSchedule? schedule, ulong endNs, Func<bool> stopRequested,
        ref long seq)
    {
        const uint runId = 1;
        int payloadSize = Math.Max(msgSize, PerfMetricHeaderSize);
        var admissions = new (Task? Admission, ulong DueNs)[sockets.Count];
        int pending = 0;
        long accepted = 0;
        var rotation = new MultiSenderRotation(sockets.Count);

        void RecordAccepted(ulong nowNs, ulong dueNs)
        {
            schedule?.Accepted(nowNs, dueNs);
            if (nowNs < endNs)
                accepted++;
        }

        void ProgressAdmissions(int waitMs, bool measureWait = true)
        {
            long waitStart = measureWait && waitMs > 0 && schedule != null
                ? Stopwatch.GetTimestamp() : 0;
            _ = pollManager.PollSockets(sockets,
                PollEventFlags.PollCompletion, waitMs);
            if (waitStart != 0)
                schedule!.RecordPollWait(waitMs, waitStart);

            for (int i = 0; i < admissions.Length; i++)
            {
                var (admission, dueNs) = admissions[i];
                if (admission is not { IsCompleted: true })
                    continue;
                admission.GetAwaiter().GetResult();
                RecordAccepted(EpochNs(), dueNs);
                admissions[i] = default;
                pending--;
            }
        }

        ulong nowNs = EpochNs();
        while (!stopRequested() && nowNs < endNs)
        {
            bool submitted = false;
            for (int visited = 0; visited < sockets.Count && pending < sockets.Count
                 && nowNs < endNs && (schedule == null || schedule.Due(nowNs))
                 && !stopRequested();)
            {
                int index = rotation.Index;
                if (admissions[index].Admission != null)
                {
                    rotation.Advance();
                    visited++;
                    continue;
                }

                using Message message = Message.Allocate(payloadSize);
                StampMetricHeader(message.AsSpan(), runId,
                    schedule == null ? PerfPhase.Active
                        : MultiLatencySample(nowNs, schedule.EpochNs,
                            latencyDurationSeconds)
                            ? PerfPhase.Latency : PerfPhase.Warmup,
                    msgSize, unchecked((ulong)++seq), nowNs);
                SendSubmission submission = PerfSocketIo.SendMeasurementAsync(
                    (IDealerSocket)sockets[index], message, SendFlags.None);
                ulong dueNs = schedule?.Submitted() ?? 0;
                nowNs = EpochNs();
                if (submission.Result == SubmitResult.Backpressured)
                {
                    admissions[index] = (submission.Admitted, dueNs);
                    pending++;
                    rotation.Advance();
                    visited++;
                    continue;
                }

                submitted = true;
                RecordAccepted(nowNs, dueNs);
                if (rotation.Accepted())
                    visited++;
            }

            int waitMs = schedule?.PollWaitMilliseconds(nowNs, endNs, pending == sockets.Count)
                ?? (submitted ? 0 : MultiPacedSchedule.MillisecondsUntil(nowNs, endNs));
            if (pending > 0)
                ProgressAdmissions(waitMs);
            else if (waitMs > 0)
                schedule!.Sleep(waitMs);

            nowNs = EpochNs();
        }

        while (pending > 0)
            ProgressAdmissions(50, measureWait: false);
        return accepted;
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
                socket.Send().Message(message).Submit();
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
