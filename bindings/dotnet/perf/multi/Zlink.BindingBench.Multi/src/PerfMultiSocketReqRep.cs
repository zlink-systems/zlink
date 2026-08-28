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
    private const uint ActivePhase = 1;
    private const int ReplyRetryPollTimeoutMs = 50;
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
        using var pollManager = new PollManager();
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
        var pollSockets = new[] { (ISocket)server };

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
            if (!ReplyReceived(pollSockets, pollManager, received, stop.Token))
                return 2;
            while (!stop.IsCancellationRequested
                   && TryReceiveNoWait(server, received))
            {
                if (!ReplyReceived(pollSockets, pollManager, received, stop.Token))
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
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
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
                    socket.Options.SendTimeout =
                        TimeSpan.FromMilliseconds(ResolveMultiSndTimeoutMs(options));
                    socket.Options.ReceiveTimeout =
                        TimeSpan.FromMilliseconds(ResolveMultiRcvTimeoutMs(options));
                }
                if (client is IConnectableSocket connectable)
                {
                    var monitor = connectable.MonitorOpen(SocketEvent.ConnectionReady);
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

            var result = await RunClientLoopAsync(slots, routerRouter, size,
                durationSeconds, latencySampleCap).ConfigureAwait(false);
            if (result.completed <= 0 || result.latencySamples.Count == 0)
            {
                DebugLogLimited(ref s_debugClientReplyLogs,
                    $"socket_reqrep_client: active failed completed={result.completed} samples={result.latencySamples.Count}");
                return 2;
            }

            var latency = ComputeLatencyStats(result.latencySamples);
            double throughput = result.completed / (double)Math.Max(1, durationSeconds);
            PrintResult(options.Pattern, options.Transport, size, throughput,
                latency.mean, latency.p95, latency.p99);
            return 0;
        }
        finally
        {
            DisposeAllQuietly(monitors);
            foreach (var client in clients)
                (client as IDisposable)?.Dispose();
        }
    }

    private static Task<(long completed, List<double> latencySamples)>
        RunClientLoopAsync(
        List<ClientSlot> slots, bool routerRouter,
        int msgSize, int durationSeconds, int latencyCap)
    {
        int payloadSize = Math.Max(msgSize, PerfMetricHeaderSize);
        using var completionPoller = Zlink.CreatePoller();
        var completionEvents = new PollEvent[slots.Count];
        for (int i = 0; i < slots.Count; i++)
            completionPoller.Add(slots[i].Socket,
                PollEventFlags.PollCompletion, (nuint)i);
        var samples = new List<double>(Math.Max(0, latencyCap));
        object gate = new();
        Exception? completionError = null;
        int hasCompletionError = 0;
        long completed = 0;
        long outstanding = 0;
        long sampleSeen = 0;
        uint rng = 0xA341316Cu;
        long deadlineTicks = DeadlineTicksFromSeconds(durationSeconds);
        ulong activeDeadlineNs = EpochNs()
            + (ulong)Math.Max(1, durationSeconds) * 1_000_000_000UL;
        TimeSpan requestTimeout = ResolveReqRepTimeout();
        bool HasCompletionError() => Volatile.Read(ref hasCompletionError) != 0;

        void RecordCompletionError(Exception ex)
        {
            lock (gate)
            {
                completionError ??= ex;
                Volatile.Write(ref hasCompletionError, 1);
            }
        }

        bool TrySubmit(ClientSlot slot)
        {
            if (HasCompletionError()
                || Stopwatch.GetTimestamp() >= deadlineTicks)
            {
                return false;
            }

            using Message message = Message.Allocate(payloadSize);
            long sentTicks = Stopwatch.GetTimestamp();
            StampMetricHeader(message.AsSpan(), RunId, PerfPhase.Active, msgSize,
                slot.NextSeq++, EpochNsFromTimestamp(sentTicks));

            try
            {
                Interlocked.Increment(ref outstanding);
                using Message? tail = PerfSocketIo.MeasurementPartCount == 2
                    ? Message.Allocate(0) : null;
                if (routerRouter)
                {
                    var request = ((IRouterSocket)slot.Socket)
                        .Request(ServerRoutingId).Message(message);
                    if (tail != null)
                        request = request.Message(tail);
                    request.Timeout(requestTimeout).Submit(SendFlags.DontWait,
                        CompleteRequest);
                }
                else
                {
                    var request = ((IDealerSocket)slot.Socket).Request()
                        .Message(message);
                    if (tail != null)
                        request = request.Message(tail);
                    request.Timeout(requestTimeout).Submit(SendFlags.DontWait,
                        CompleteRequest);
                }
            }
            catch (ZlinkException ex)
                when (PerfShared.IsTransientBackpressure(ex.NativeErrno)
                      || PerfShared.IsTransientNetworkError(ex.NativeErrno))
            {
                Interlocked.Decrement(ref outstanding);
                return false;
            }

            if (s_debugEnabled)
                DebugLogLimited(ref s_debugClientSubmitLogs,
                    $"socket_reqrep_client: submitted seq={slot.NextSeq - 1}");
            return true;
        }

        void CompleteRequest(RequestResult result,
            IReadOnlyList<Message> parts)
        {
            try
            {
                if (result == RequestResult.TimedOut)
                {
                    if (s_debugEnabled)
                        DebugLogLimited(ref s_debugClientReplyLogs,
                            "socket_reqrep_client: completion timed out");
                    return;
                }
                if (result != RequestResult.Ok)
                    throw new InvalidOperationException(
                        $"request completion failed: {result}");
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
                    && header.Phase == ActivePhase)
                {
                    ulong nowNs = EpochNs();
                    if (nowNs < activeDeadlineNs && nowNs >= header.SentTsNs)
                    {
                        lock (gate)
                        {
                            double sampleNs =
                                (nowNs - header.SentTsNs) * 0.5;
                            ReservoirSample(samples, sampleNs,
                                ref sampleSeen, latencyCap, ref rng);
                        }
                        Interlocked.Increment(ref completed);
                    }
                }
            }
            catch (Exception ex)
            {
                RecordCompletionError(ex);
            }
            finally
            {
                Zlink.MultipartClose(parts);
                Interlocked.Decrement(ref outstanding);
            }
        }

        while (Stopwatch.GetTimestamp() < deadlineTicks && !HasCompletionError())
        {
            bool submittedAny = false;
            for (int i = 0; i < slots.Count; i++)
                submittedAny |= TrySubmit(slots[i]);

            long remainingTicks = deadlineTicks - Stopwatch.GetTimestamp();
            int remainingMs = remainingTicks <= 0 ? 0 : (int)Math.Min(
                int.MaxValue, Math.Ceiling(remainingTicks * 1000.0
                    / Stopwatch.Frequency));
            int waitMs = submittedAny ? 0 : Math.Min(50, remainingMs);
            if (waitMs >= 0)
                _ = completionPoller.Wait(completionEvents,
                    TimeSpan.FromMilliseconds(waitMs));
        }

        long drainStart = Stopwatch.GetTimestamp();
        TimeSpan drainTimeout = ResolveReqRepDrainTimeout();
        while (!HasCompletionError() && Volatile.Read(ref outstanding) > 0)
        {
            _ = completionPoller.Wait(completionEvents,
                TimeSpan.FromMilliseconds(50));
            if (Stopwatch.GetElapsedTime(drainStart) > drainTimeout)
                throw new TimeoutException("multi request/reply operations did not drain");
        }

        lock (gate)
        {
            if (completionError != null)
                throw completionError;
        }
        return Task.FromResult((Volatile.Read(ref completed), samples));
    }

    private static bool ReplyReceived(IReadOnlyList<ISocket> pollSockets,
        PollManager pollManager, Received received, CancellationToken stopToken)
    {
        if (!PerfSocketIo.TryMeasurementPayload(received.Parts,
                out Message payloadPart))
            return true;
        if (!received.RequestSeq.HasValue)
            return true;

        int payloadSize = payloadPart.Size;
        ulong requestSeq = received.RequestSeq.Value;
        if (s_debugEnabled)
        {
            DebugLogLimited(ref s_debugServerRecvLogs,
                $"socket_reqrep_server: recv size={payloadSize} seq={requestSeq}");
        }
        using Message replyTemplate = Message.Allocate(payloadSize);
        payloadPart.AsReadOnlySpan().CopyTo(replyTemplate.AsSpan());
        while (!stopToken.IsCancellationRequested)
        {
            using Message reply = replyTemplate.Copy();
            try
            {
                // Keep a template and submit a fresh copy on every attempt,
                // matching the C retry contract when backpressure consumes a
                // native message part.
                using Message tail = Message.Allocate(0);
                if (PerfSocketIo.MeasurementPartCount == 2)
                    received.Reply().Message(reply).Message(tail).Submit();
                else
                    received.Reply().Message(reply).Submit();
                break;
            }
            catch (ZlinkSubmitException ex)
                when (PerfShared.IsTransientBackpressure(ex.NativeErrno))
            {
                _ = pollManager.PollSockets(pollSockets, SocketPollOut,
                    ReplyRetryPollTimeoutMs);
            }
        }
        if (s_debugEnabled)
        {
            DebugLogLimited(ref s_debugServerReplyLogs,
                $"socket_reqrep_server: replied size={payloadSize} seq={requestSeq}");
        }
        return !stopToken.IsCancellationRequested;
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
    }

    private static void DebugLogLimited(ref int counter, string message)
    {
        if (!s_debugEnabled)
            return;
        if (Interlocked.Increment(ref counter) <= 16)
            Console.Error.WriteLine(message);
    }
}
