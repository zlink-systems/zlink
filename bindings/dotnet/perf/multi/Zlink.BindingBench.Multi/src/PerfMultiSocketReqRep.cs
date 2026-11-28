using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiSocketReqRep
{
    private static readonly RoutingId ServerRoutingId = RoutingId.From("SERVER"u8);
    private const uint RunId = 1;
    private static int s_debugServerRecvLogs;
    private static int s_debugServerReplyLogs;
    private static int s_debugClientSubmitLogs;
    private static int s_debugClientReplyLogs;
    private static readonly bool s_debugEnabled =
        !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PERF_DEBUG"));

    internal static int RunDealerRouterServer(PerfOptions options)
    {
        return RunServer(options, routerRouter: false, "multi-dealer-router-reqrep");
    }

    internal static Task<int> RunDealerRouterClient(PerfOptions options)
    {
        return RunClientAsync(options, routerRouter: false);
    }

    internal static int RunRouterRouterServer(PerfOptions options)
    {
        return RunServer(options, routerRouter: true, "multi-router-router-reqrep");
    }

    internal static Task<int> RunRouterRouterClient(PerfOptions options)
    {
        return RunClientAsync(options, routerRouter: true);
    }

    private static int RunServer(PerfOptions options, bool routerRouter,
        string endpointName)
    {
        int size = Math.Max(1, options.Size);
        int rcvTimeoutMs = ResolveMultiRcvTimeoutMs(options);
        string endpoint = MultiEndpointFor(options.Transport, endpointName, options);

        using var ctx = Zlink.CreateContext();
        ApplyMultiServerContextOptions(ctx, options);
        using var server = ctx.CreateRouterSocket();
        ApplyMultiSocketOptions(server, options);
        ConfigureTlsServerIfNeeded(server, options.Transport);
        if (routerRouter)
            server.SetRoutingId(ServerRoutingId);

        server.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeoutMs);
        // Match the C request/reply server: configure the message unit before
        // bind, then recalculate the socket policy before advertising READY.
        // The server receives as soon as clients connect and does not gate on
        // a connection-ready event count.
        server.Bind(endpoint);
        endpoint = server.Options.LastEndpoint;
        RecalculateAutoHwm(ctx);
        PrintAutoHwmSnapshot(server, "server", options.Transport, size);
        WriteStdoutLine($"READY,{endpoint}");
        using var stop = new CancellationTokenSource();
        Thread stdinThread = new(() =>
        {
            string? line;
            while ((line = Console.In.ReadLine()) != null)
            {
                if (line == "STOP" || line == "QUIT")
                {
                    stop.Cancel();
                    break;
                }
            }
        })
        {
            IsBackground = true,
            Name = "multi socket reqrep server control"
        };
        stdinThread.Start();

        using var received = Received.Create();
        while (!stop.IsCancellationRequested)
        {
            if (!TryReceiveBlocking(server, received))
                continue;
            if (!ReplyReceived(received))
                return 2;
            while (!stop.IsCancellationRequested
                   && TryReceiveNoWait(server, received))
            {
                if (!ReplyReceived(received))
                    return 2;
            }
        }

        return 0;
    }

    private static async Task<int> RunClientAsync(PerfOptions options,
        bool routerRouter)
    {
        int size = Math.Max(1, options.Size);
        int durationSeconds = ResolveMultiDurationSeconds(options);
        int latencySeconds = ResolveMultiLatencyDurationSeconds(options);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        ulong monitorHwmBytes = ResolveMultiMonitorHwmBytes();
        int latencySampleCap = ResolveMultiLatencySampleCap(options);
        int clientCount = ResolveMultiClients(options);
        string endpoint = options.Endpoint;

        using var ctx = Zlink.CreateContext();
        using var pollManager = new PollManager();
        ApplyMultiClientContextOptions(ctx, options);
        var clients = new List<IZlinkSocket>(clientCount);
        var monitors = new List<MonitorSocket>(clientCount);
        var slots = new List<ClientSlot>(clientCount);
        try
        {
            for (int i = 0; i < clientCount; i++)
            {
                IZlinkSocket client;
                if (routerRouter)
                {
                    var router = ctx.CreateRouterSocket();
                    // Match the C request/reply harness: the client routing
                    // identity and the peer routing identity are configured
                    // before Connect so routed readiness has the same meaning.
                    router.SetRoutingId(RoutingId.From(
                        System.Text.Encoding.ASCII.GetBytes($"client_{i}")));
                    router.Options.SetConnectRoutingId(ServerRoutingId);
                    client = router;
                }
                else
                {
                    var dealer = ctx.CreateDealerSocket();
                    dealer.SetRoutingId(RoutingId.From(
                        System.Text.Encoding.ASCII.GetBytes($"CLIENT-{i}")));
                    client = dealer;
                }

                if (client is ISocket socket)
                {
                    ApplyMultiSocketOptions(socket, options);
                    ConfigureTlsClientIfNeeded(socket, options.Transport);
                    socket.Options.ReceiveTimeout =
                        TimeSpan.FromMilliseconds(ResolveMultiRcvTimeoutMs(options));
                }
                if (client is IConnectableSocket connectable)
                {
                    var monitor = connectable.MonitorOpen(
                        SocketEvent.ConnectionReady, monitorHwmBytes);
                    connectable.Connect(endpoint);
                    monitors.Add(monitor);
                }
                clients.Add(client);
            }

            var socketList = new List<ISocket>(clients.Count);
            foreach (var client in clients)
                socketList.Add((ISocket)client);
            List<ISocket> activeClients = WaitClientConnectReadyAll(
                pollManager, socketList, monitors, readyTimeoutMs);
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
                PrintAutoHwmSnapshot((ISocket)clients[0], "endpoint",
                    options.Transport, size);

            for (int i = 0; i < clients.Count; i++)
            {
                var slot = new ClientSlot(clients[i]);
                slots.Add(slot);
            }

            var active = await RunClientWindowAsync(slots, routerRouter,
                size, durationSeconds, latencySeconds, latencySampleCap,
                PerfPhase.Active, null).ConfigureAwait(false);
            if (active.completed <= 0 || active.accepted <= 0)
            {
                DebugLogLimited(ref s_debugClientReplyLogs,
                    $"socket_reqrep_client: active failed completed={active.completed} accepted={active.accepted}");
                return 2;
            }
            var schedule = new MultiPacedSchedule(
                MultiTargetRate(active.completed, durationSeconds));
            var paced = await RunClientWindowAsync(slots, routerRouter,
                size, durationSeconds, latencySeconds, latencySampleCap,
                PerfPhase.Latency, schedule).ConfigureAwait(false);
            PrintMultiBenchDiag(options.Pattern, size, active.accepted,
                durationSeconds, schedule, active.completed,
                paced.pacedReceived / (double)latencySeconds);
            if (paced.latencyCount <= 0)
                return 2;

            var latency = ComputeMultiLatencyStats(paced.latencySamples,
                paced.latencyCount, paced.latencySum);
            double throughput = active.completed / (double)Math.Max(1, durationSeconds);
            PrintResult(options.Pattern, options.Transport, size, throughput,
                latency.mean, latency.p95, latency.p99);
            // PERF_MULTI_TEST_POLICY.md:379,386-388 / PERF_POLICY.md:483-486 —
            // emit CLIENT_DONE, keep the request completion target sockets
            // open, and close them only after the runner has stopped the
            // server and sent STOP. C reference:
            // bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:792,:722-731.
            WriteStdoutLine($"CLIENT_DONE,{size}");
            if (!WaitForRunnerStopAfterDone())
                return 2;
            return 0;
        }
        finally
        {
            DisposeAllQuietly(monitors);
            foreach (var client in clients)
                (client as IDisposable)?.Dispose();
        }
    }

    private static Task<(long completed, long accepted, long pacedReceived,
        List<double> latencySamples, long latencyCount, double latencySum)>
        RunClientWindowAsync(List<ClientSlot> slots, bool routerRouter,
        int msgSize, int durationSeconds, int latencySeconds, int latencyCap,
        PerfPhase phase, MultiPacedSchedule? schedule)
    {
        int payloadSize = Math.Max(msgSize, PerfMetricHeaderSize);
        var samples = new List<double>(Math.Max(0, latencyCap));
        object gate = new();
        Exception? completionError = null;
        int hasCompletionError = 0;
        long pendingRequests = 0;
        long completed = 0;
        long accepted = 0;
        long pacedReceived = 0;
        long sampleSeen = 0;
        double sampleSum = 0.0;
        uint rng = 0xA341316Cu;
        long deadlineTicks = Stopwatch.GetTimestamp() + (long)(
            (schedule == null ? durationSeconds : latencySeconds + 0.5)
            * Stopwatch.Frequency);
        ulong deadlineNs = EpochNsFromTimestamp(deadlineTicks);
        TimeSpan requestTimeout = ResolveReqRepTimeout();
        using var completionPoller = Zlink.CreatePoller();
        var completionEvents = new PollEvent[Math.Max(1, slots.Count)];
        for (int i = 0; i < slots.Count; i++)
        {
            completionPoller.Add(slots[i].Socket,
                PollEventFlags.PollCompletion, (nuint)i);
        }
        bool HasCompletionError() => Volatile.Read(ref hasCompletionError) != 0;

        void RecordAdmission(ulong dueNs)
        {
            long nowTicks = Stopwatch.GetTimestamp();
            if (schedule != null)
            {
                lock (schedule)
                    schedule.Accepted(EpochNsFromTimestamp(nowTicks), dueNs);
            }
            if (nowTicks < deadlineTicks)
                Interlocked.Increment(ref accepted);
        }

        void RecordCompletionError(Exception ex)
        {
            lock (gate)
            {
                completionError ??= ex;
                Volatile.Write(ref hasCompletionError, 1);
            }
        }

        RequestSubmission Submit(ClientSlot slot)
        {
            using Message message = Message.Allocate(payloadSize);
            long sentTicks = Stopwatch.GetTimestamp();
            ulong seq = slot.NextSeq;
            StampMetricHeader(message.AsSpan(), RunId, phase,
                msgSize, seq, EpochNsFromTimestamp(sentTicks));
            using Message? tail = PerfSocketIo.MeasurementPartCount == 2
                ? Message.Allocate(0) : null;
            RequestSubmission submission;
            if (routerRouter)
            {
                var request = ((IRouterSocket)slot.Socket)
                    .Request(ServerRoutingId).Message(message);
                if (tail != null)
                    request = request.Message(tail);
                submission = request.Timeout(requestTimeout).Async();
            }
            else
            {
                var request = ((IDealerSocket)slot.Socket).Request()
                    .Message(message);
                if (tail != null)
                    request = request.Message(tail);
                submission = request.Timeout(requestTimeout).Async();
            }
            slot.NextSeq = seq + 1;
            if (s_debugEnabled)
                DebugLogLimited(ref s_debugClientSubmitLogs,
                    $"socket_reqrep_client: submitted seq={seq}");
            return submission;
        }

        async Task ObserveRequestAsync(ClientSlot slot,
            RequestSubmission submission, ulong dueNs)
        {
            // Retire the request after both stages have been observed.
            if (submission.Result == SubmitResult.Backpressured)
            {
                try
                {
                    await submission.Admitted.ConfigureAwait(false);
                    RecordAdmission(dueNs);
                }
                catch (Exception ex)
                {
                    RecordCompletionError(ex);
                }
                finally
                {
                    slot.CompleteAdmission();
                }
            }

            IReadOnlyList<Message>? parts = null;
            try
            {
                parts = await submission.Reply.ConfigureAwait(false);
                if (s_debugEnabled)
                {
                    DebugLogLimited(ref s_debugClientReplyLogs,
                        $"socket_reqrep_client: completion parts={parts.Count}");
                }
                if (PerfSocketIo.TryMeasurementPayload(parts,
                        out Message replyPayload)
                    && PerfShared.TryDecodeMetricHeader(
                        replyPayload.AsReadOnlySpan(),
                        out PerfMetricHeader header)
                    && header.RunId == RunId
                    && header.MsgSize == (uint)msgSize
                    && header.Phase == (uint)phase)
                {
                    long completionTicks = Stopwatch.GetTimestamp();
                    ulong nowNs = EpochNsFromTimestamp(completionTicks);
                    if (schedule != null && MultiLatencySample(nowNs,
                            schedule.EpochNs, latencySeconds))
                        Interlocked.Increment(ref pacedReceived);
                    if (nowNs >= header.SentTsNs
                        && (schedule == null
                            ? completionTicks < deadlineTicks
                            : MultiLatencySample(header.SentTsNs,
                                schedule.EpochNs, latencySeconds)))
                    {
                        if (schedule != null)
                        {
                            lock (gate)
                            {
                                double sampleNs =
                                    (nowNs - header.SentTsNs) * 0.5;
                                ReservoirSampleMulti(samples, sampleNs,
                                    ref sampleSeen, ref sampleSum, latencyCap,
                                    ref rng);
                            }
                        }
                        Interlocked.Increment(ref completed);
                    }
                }
            }
            catch (ZlinkRequestException ex)
                when (ex.Result == ZlinkRequestException.ErrorCode.TimedOut)
            {
                if (s_debugEnabled)
                    DebugLogLimited(ref s_debugClientReplyLogs,
                        "socket_reqrep_client: completion timed out");
            }
            catch (Exception ex)
            {
                RecordCompletionError(ex);
            }
            finally
            {
                if (parts != null)
                    Zlink.MultipartClose(parts);
                Interlocked.Decrement(ref pendingRequests);
            }
        }

        // Submit once per socket per turn, then explicitly progress both
        // admission and reply completions. This preserves continuous load
        // without letting one socket starve the completion owner.
        int roundStart = 0;
        while (Stopwatch.GetTimestamp() < deadlineTicks && !HasCompletionError())
        {
            bool submittedAny = false;
            int start = roundStart;
            int attempts = 0;
            for (; attempts < slots.Count; attempts++)
            {
                if (schedule != null)
                {
                    ulong nowNs = EpochNs();
                    if (nowNs >= deadlineNs || !schedule.Due(nowNs))
                        break;
                }
                ClientSlot slot = schedule == null ? slots[attempts]
                    : slots[(start + attempts) % slots.Count];
                if (slot.AdmissionPending)
                    continue;
                try
                {
                    RequestSubmission submission = Submit(slot);
                    ulong dueNs = schedule?.Submitted() ?? 0;
                    if (submission.Result == SubmitResult.Ok)
                        RecordAdmission(dueNs);
                    else if (submission.Result == SubmitResult.Backpressured)
                        slot.BeginAdmission();
                    Interlocked.Increment(ref pendingRequests);
                    _ = ObserveRequestAsync(slot, submission, dueNs);
                    submittedAny = true;
                }
                catch (Exception ex)
                {
                    RecordCompletionError(ex);
                }
            }

            if (schedule != null && slots.Count > 0)
                roundStart = (start + 1) % slots.Count;

            int waitMs = 0;
            if (!submittedAny)
            {
                waitMs = schedule != null && attempts < slots.Count
                    ? schedule.WaitMilliseconds(EpochNs(), deadlineNs)
                    : RemainingPollTimeoutMs(deadlineTicks);
            }
            _ = completionPoller.Wait(completionEvents,
                TimeSpan.FromMilliseconds(waitMs));
        }

        long drainDeadline = Stopwatch.GetTimestamp()
            + (long)(ResolveReqRepDrainTimeout().TotalSeconds
                * Stopwatch.Frequency);
        while (Volatile.Read(ref pendingRequests) > 0)
        {
            int waitMs = RemainingPollTimeoutMs(drainDeadline);
            if (waitMs <= 0)
                throw new TimeoutException(
                    "multi request/reply operations did not drain");
            _ = completionPoller.Wait(completionEvents,
                TimeSpan.FromMilliseconds(waitMs));
        }

        lock (gate)
        {
            if (completionError != null)
                throw completionError;
        }
        return Task.FromResult((Volatile.Read(ref completed),
            Volatile.Read(ref accepted), Volatile.Read(ref pacedReceived),
            samples, sampleSeen, sampleSum));
    }

    private static int RemainingPollTimeoutMs(long deadlineTicks)
    {
        long remainingTicks = deadlineTicks - Stopwatch.GetTimestamp();
        if (remainingTicks <= 0)
            return 0;
        double remainingMs = remainingTicks * 1000.0 / Stopwatch.Frequency;
        return Math.Min(50, Math.Max(1, (int)Math.Ceiling(remainingMs)));
    }

    /// <summary>
    ///     Blocks until the runner sends STOP/QUIT after CLIENT_DONE. Mirrors
    ///     bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:722-731.
    /// </summary>
    private static bool WaitForRunnerStopAfterDone()
    {
        string? line;
        while ((line = Console.In.ReadLine()) != null)
        {
            if (line == "STOP" || line == "QUIT")
                return true;
        }
        return false;
    }

    private static bool ReplyReceived(Received received)
    {
        if (!PerfSocketIo.TryMeasurementPayload(received.Parts,
                out Message payloadPart))
            return true;
        if (received.ReplyToken == null)
            return true;

        int payloadSize = payloadPart.Size;
        if (s_debugEnabled)
        {
            DebugLogLimited(ref s_debugServerRecvLogs,
                $"socket_reqrep_server: recv size={payloadSize}");
        }
        try
        {
            if (PerfSocketIo.MeasurementPartCount == 2)
            {
                using Message tail = Message.Allocate(0);
                // ReplyCore takes a native ref-counted clone before submit.
                // Forward the received Message directly so the benchmark does
                // not add a managed payload allocation and byte copy that are
                // absent from the public request/reply path itself.
                received.Reply().Message(payloadPart).Message(tail).Submit();
            }
            else
            {
                received.Reply().Message(payloadPart).Submit();
            }
        }
        catch (ZlinkSubmitException ex) when (IsStaleRoute(ex))
        {
            // The client owns benchmark completion and may close immediately
            // after printing its result. A request already received by this
            // server can therefore lose its route during graceful teardown.
            return true;
        }
        catch (ZlinkSubmitException ex)
        {
            if (s_debugEnabled)
            {
                Console.Error.WriteLine(
                    $"socket_reqrep_server: reply failed result={ex.Result} errno={ex.NativeErrno}: {ex.Message}");
            }
            return false;
        }
        if (s_debugEnabled)
        {
            DebugLogLimited(ref s_debugServerReplyLogs,
                $"socket_reqrep_server: replied size={payloadSize}");
        }
        return true;
    }

    private static bool IsStaleRoute(ZlinkSubmitException error)
    {
        return error.Result == ZlinkSubmitException.ErrorCode.NotConnected
            || error.Result == ZlinkSubmitException.ErrorCode.NotFound;
    }

    private static bool TryReceiveBlocking(IRouterSocket receiver, Received result)
    {
        try
        {
            return receiver.Recv(result);
        }
        catch (ZlinkRecvException ex) when (PerfShared.IsInterrupted(ex.NativeErrno)
                                            || PerfShared.IsWouldBlock(ex.NativeErrno))
        {
            return false;
        }
        catch (ZlinkException ex) when (PerfShared.IsInterrupted(ex.NativeErrno)
                                        || PerfShared.IsWouldBlock(ex.NativeErrno))
        {
            return false;
        }
    }

    private static bool TryReceiveNoWait(IRouterSocket receiver, Received result)
    {
        try
        {
            return receiver.Recv(result, RecvFlags.DontWait);
        }
        catch (ZlinkRecvException ex) when (PerfShared.IsInterrupted(ex.NativeErrno)
                                            || PerfShared.IsWouldBlock(ex.NativeErrno))
        {
            return false;
        }
        catch (ZlinkException ex) when (PerfShared.IsInterrupted(ex.NativeErrno)
                                        || PerfShared.IsWouldBlock(ex.NativeErrno))
        {
            return false;
        }
    }

    private static bool TryGetPayloadPart(Received received,
        out Message payloadPart)
    {
        if (received.IsSinglePart)
        {
            payloadPart = received.FirstPart();
            return true;
        }
        if (received.Parts.Count > 0)
        {
            payloadPart = received.Parts[received.Parts.Count - 1];
            return true;
        }
        payloadPart = default!;
        return false;
    }

    private static TimeSpan ResolveReqRepTimeout()
    {
        int ms = PerfEnv.ReadPositive("PERF_MULTI_REQREP_TIMEOUT_MS", 200);
        return TimeSpan.FromMilliseconds(ms);
    }

    private static TimeSpan ResolveReqRepDrainTimeout()
    {
        int ms = PerfEnv.ReadPositive("PERF_MULTI_REQREP_DRAIN_TIMEOUT_MS", 5000);
        return TimeSpan.FromMilliseconds(ms);
    }

    private sealed class ClientSlot
    {
        internal ClientSlot(IZlinkSocket socket)
        {
            Socket = socket;
        }

        internal IZlinkSocket Socket { get; }
        internal ulong NextSeq { get; set; } = 1;
        private int _admissionPending;

        internal bool AdmissionPending =>
            Volatile.Read(ref _admissionPending) != 0;

        internal void BeginAdmission()
        {
            if (Interlocked.Exchange(ref _admissionPending, 1) != 0)
                throw new InvalidOperationException(
                    "The socket already has a pending admission.");
        }

        internal void CompleteAdmission()
        {
            Volatile.Write(ref _admissionPending, 0);
        }

    }

    private static void DebugLogLimited(ref int counter, string message)
    {
        if (!s_debugEnabled)
            return;
        if (Interlocked.Increment(ref counter) <= 16)
            Console.Error.WriteLine(message);
    }
}
