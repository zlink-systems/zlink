using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfReqRep
{
    private static readonly RoutingId RouterReqRepServerRid =
        RoutingId.From("SERVER"u8);
    private static readonly RoutingId RouterReqRepClientRid =
        RoutingId.From("CLIENT"u8);
    private const uint RunId = 1;
    private const uint ActivePhase = 1;

    internal static int RunDealerRouter(string transport, int size)
    {
        int durationSeconds = ResolveSingleDurationSeconds();
        int readyTimeoutMs = ResolveSingleConnectReadyTimeoutMs();
        int latencySampleCap = ResolveSingleLatencyCount("DEALER_ROUTER_REQREP");

        using var ctx = Zlink.CreateContext();
        ApplySingleContextOptions(ctx);
        using var server = ctx.CreateRouterSocket();
        using var client = ctx.CreateDealerSocket();
        ApplySingleAutoHwmMsgUnit(ctx, size);
        RecalculateSingleAutoHwm(ctx);
        ApplySingleSocketOptions(server);
        ApplySingleSocketOptions(client);
        ConfigureTlsServerIfNeeded(server, transport);
        ConfigureTlsClientIfNeeded(client, transport);
        MonitorSocket? serverMonitor = null;
        MonitorSocket? clientMonitor = null;
        string endpoint = EndpointFor(transport, "dealer-router-reqrep");

        try
        {
            serverMonitor = server.MonitorOpen(SocketEvent.ConnectionReady);
            clientMonitor = client.MonitorOpen(SocketEvent.ConnectionReady);
            client.SetRoutingId(RoutingId.From("DEALER-REQ"u8));
            server.Bind(endpoint);
            endpoint = server.Options.LastEndpoint;
            client.Connect(endpoint);
            if (!(WaitForConnectionReadyWithActivity(serverMonitor, server,
                    readyTimeoutMs, acceptAccepted: false)
                && WaitForConnectionReady(clientMonitor, readyTimeoutMs)))
            {
                DebugLog("single_dealer_router_reqrep_error:connection_not_ready");
                TryCleanup(client, server, endpoint, "[single-dealer-router-reqrep]");
                return 2;
            }

            EmitSingleAutoHwmDetail(serverMonitor, "DEALER_ROUTER_REQREP",
                transport, "replier", "router", size);
            EmitSingleAutoHwmDetail(clientMonitor, "DEALER_ROUTER_REQREP",
                transport, "requester", "dealer", size);

            serverMonitor.Dispose();
            serverMonitor = null;
            clientMonitor.Dispose();
            clientMonitor = null;

            var result = RunDealerRouterActive(client, server, size,
                durationSeconds, latencySampleCap);
            if (result.completed <= 0 || result.latencySamples.Count == 0)
            {
                DebugLog("single_dealer_router_reqrep_error:active_failed");
                TryCleanup(client, server, endpoint, "[single-dealer-router-reqrep]");
                return 2;
            }

            double throughput = result.completed / (double)Math.Max(durationSeconds, 1);
            var latency = ComputeLatencyStats(result.latencySamples);
            PrintResult("DEALER_ROUTER_REQREP", transport, size, throughput,
                latency.mean, latency.p95, latency.p99, bandwidthMultiplier: 2.0);
            TryCleanup(client, server, endpoint, "[single-dealer-router-reqrep]");
            return 0;
        }
        catch (Exception ex)
        {
            TryCleanup(client, server, endpoint, "[single-dealer-router-reqrep]");
            DebugLog($"single_dealer_router_reqrep_error:exception:{ex}");
            return 2;
        }
        finally
        {
            serverMonitor?.Dispose();
            clientMonitor?.Dispose();
        }
    }

    internal static int RunRouterRouter(string transport, int size)
    {
        int durationSeconds = ResolveSingleDurationSeconds();
        int readyTimeoutMs = ResolveSingleConnectReadyTimeoutMs();
        int latencySampleCap = ResolveSingleLatencyCount("ROUTER_ROUTER_REQREP");

        using var ctx = Zlink.CreateContext();
        ApplySingleContextOptions(ctx);
        using var server = ctx.CreateRouterSocket();
        using var client = ctx.CreateRouterSocket();
        ApplySingleAutoHwmMsgUnit(ctx, size);
        RecalculateSingleAutoHwm(ctx);
        ApplySingleSocketOptions(server);
        ApplySingleSocketOptions(client);
        ConfigureTlsServerIfNeeded(server, transport);
        ConfigureTlsClientIfNeeded(client, transport);
        MonitorSocket? serverMonitor = null;
        MonitorSocket? clientMonitor = null;
        string endpoint = EndpointFor(transport, "router-router-reqrep");

        try
        {
            server.SetRoutingId(RouterReqRepServerRid);
            client.SetRoutingId(RouterReqRepClientRid);
            server.Options.Mandatory = true;
            client.Options.Mandatory = true;
            serverMonitor = server.MonitorOpen(SocketEvent.ConnectionReady);
            clientMonitor = client.MonitorOpen(SocketEvent.ConnectionReady);
            server.Bind(endpoint);
            endpoint = server.Options.LastEndpoint;
            client.Connect(endpoint);
            if (!(WaitForConnectionReady(serverMonitor, readyTimeoutMs)
                && WaitForConnectionReady(clientMonitor, readyTimeoutMs)))
            {
                DebugLog("single_router_router_reqrep_error:connection_not_ready");
                TryCleanup(client, server, endpoint, "[single-router-router-reqrep]");
                return 2;
            }

            if (!CompleteRouterRouterHandshake(client, server, readyTimeoutMs,
                    out RoutingId targetRoutingId))
            {
                DebugLog("single_router_router_reqrep_error:route_probe_failed");
                TryCleanup(client, server, endpoint, "[single-router-router-reqrep]");
                return 2;
            }

            EmitSingleAutoHwmDetail(serverMonitor, "ROUTER_ROUTER_REQREP",
                transport, "replier", "router", size);
            EmitSingleAutoHwmDetail(clientMonitor, "ROUTER_ROUTER_REQREP",
                transport, "requester", "router", size);

            serverMonitor.Dispose();
            serverMonitor = null;
            clientMonitor.Dispose();
            clientMonitor = null;

            var result = RunRouterRouterActive(client, server,
                targetRoutingId, size, durationSeconds, latencySampleCap);
            if (result.completed <= 0 || result.latencySamples.Count == 0)
            {
                DebugLog("single_router_router_reqrep_error:active_failed");
                TryCleanup(client, server, endpoint, "[single-router-router-reqrep]");
                return 2;
            }

            double throughput = result.completed / (double)Math.Max(durationSeconds, 1);
            var latency = ComputeLatencyStats(result.latencySamples);
            PrintResult("ROUTER_ROUTER_REQREP", transport, size, throughput,
                latency.mean, latency.p95, latency.p99, bandwidthMultiplier: 2.0);
            TryCleanup(client, server, endpoint, "[single-router-router-reqrep]");
            return 0;
        }
        catch (Exception ex)
        {
            TryCleanup(client, server, endpoint, "[single-router-router-reqrep]");
            DebugLog($"single_router_router_reqrep_error:exception:{ex}");
            return 2;
        }
        finally
        {
            serverMonitor?.Dispose();
            clientMonitor?.Dispose();
        }
    }

    private static (long completed, List<double> latencySamples) RunDealerRouterActive(
        IDealerSocket client, IRouterSocket server, int msgSize,
        int durationSeconds, int latencyCap)
    {
        using var stop = new ManualResetEventSlim(false);
        Exception? serverError = null;
        long serverReceived = 0;
        long serverReplied = 0;
        var serverThread = new Thread(() =>
        {
            try
            {
                RunReplyLoop(server, stop, ref serverReceived, ref serverReplied);
            }
            catch (Exception ex)
            {
                serverError = ex;
            }
        })
        {
            IsBackground = true,
            Name = "single dealer-router reqrep replier"
        };
        serverThread.Start();

        var result = RunRequestLoop(
            client,
            msgSize,
            durationSeconds,
            latencyCap,
            (message, callback) => client.Request()
                .Message(message)
                .Timeout(ResolveReqRepTimeout())
                .Flags(SendFlags.DontWait)
                .Submit(callback));
        stop.Set();
        if (!SendStopTokenBlocking(client, "[single-dealer-router-reqrep]"))
            throw new TimeoutException("dealer-router reqrep stop token was not sent");
        serverThread.Join();
        if (serverError != null)
            throw serverError;
        DebugLog(
            $"single_dealer_router_reqrep_debug:server_received={serverReceived}:server_replied={serverReplied}:completed={result.completed}");
        return result;
    }

    private static bool CompleteRouterRouterHandshake(IRouterSocket client,
        IRouterSocket server, int timeoutMs, out RoutingId targetRoutingId)
    {
        targetRoutingId = RouterReqRepServerRid;
        ReadOnlySpan<byte> ping = "PING"u8;
        ReadOnlySpan<byte> pong = "PONG"u8;
        RoutingId? clientActualRoutingId = null;

        using var poller = Zlink.CreatePoller();
        var events = new PollEvent[1];
        poller.Add(server, PollEventFlags.PollIn, 0);
        long deadlineTicks =
            DeadlineTicksFromMilliseconds(Math.Max(1000, timeoutMs));
        while (clientActualRoutingId == null
               && Stopwatch.GetTimestamp() < deadlineTicks)
        {
            try
            {
                _ = PerfSocketIo.Send(client, RouterReqRepServerRid, ping,
                    SendFlags.DontWait);
            }
            catch (ZlinkException ex)
                when (PerfShared.IsTransientBackpressure(ex.NativeErrno)
                      || PerfShared.IsTransientNetworkError(ex.NativeErrno))
            {
            }

            int waitMs = Math.Max(1,
                (int)Math.Ceiling((deadlineTicks - Stopwatch.GetTimestamp())
                    * 1000.0 / Stopwatch.Frequency));
            if (!WaitForInput(poller, events, waitMs))
                continue;

            using var received = Received.Create();
            while (TryReceiveBlocking(server, received))
            {
                if (received.RoutingId == null
                    || !IsPayload(received, ping))
                {
                    continue;
                }

                clientActualRoutingId = received.RoutingId.Value;
                break;
            }
        }

        if (clientActualRoutingId == null)
            return false;

        try
        {
            if (PerfSocketIo.Send(server, clientActualRoutingId.Value, pong,
                    SendFlags.None) == 0)
            {
                return false;
            }
        }
        catch (ZlinkException ex)
            when (PerfShared.IsTransientBackpressure(ex.NativeErrno)
                  || PerfShared.IsTransientNetworkError(ex.NativeErrno))
        {
            return false;
        }

        using var clientPoller = Zlink.CreatePoller();
        var clientEvents = new PollEvent[1];
        clientPoller.Add(client, PollEventFlags.PollIn, 0);
        while (Stopwatch.GetTimestamp() < deadlineTicks)
        {
            int waitMs = Math.Max(1,
                (int)Math.Ceiling((deadlineTicks - Stopwatch.GetTimestamp())
                    * 1000.0 / Stopwatch.Frequency));
            if (!WaitForInput(clientPoller, clientEvents, waitMs))
                continue;

            using var response = Received.Create();
            while (TryReceiveBlocking(client, response))
            {
                if (response.RoutingId == null || !IsPayload(response, pong))
                    continue;

                targetRoutingId = response.RoutingId.Value;
                return true;
            }
        }

        return false;
    }

    private static (long completed, List<double> latencySamples) RunRouterRouterActive(
        IRouterSocket client, IRouterSocket server, RoutingId targetRid,
        int msgSize, int durationSeconds, int latencyCap)
    {
        using var stop = new ManualResetEventSlim(false);
        Exception? serverError = null;
        long serverReceived = 0;
        long serverReplied = 0;
        var serverThread = new Thread(() =>
        {
            try
            {
                RunReplyLoop(server, stop, ref serverReceived, ref serverReplied);
            }
            catch (Exception ex)
            {
                serverError = ex;
            }
        })
        {
            IsBackground = true,
            Name = "single router-router reqrep replier"
        };
        serverThread.Start();

        var result = RunRequestLoop(
            client,
            msgSize,
            durationSeconds,
            latencyCap,
            (message, callback) => client.Request(targetRid)
                .Message(message)
                .Timeout(ResolveReqRepTimeout())
                .Flags(SendFlags.DontWait)
                .Submit(callback));
        stop.Set();
        if (!SendRoutedStopTokenBlocking(client, targetRid,
                "[single-router-router-reqrep]"))
        {
            throw new TimeoutException("router-router reqrep stop token was not sent");
        }
        serverThread.Join();
        if (serverError != null)
            throw serverError;
        DebugLog(
            $"single_router_router_reqrep_debug:server_received={serverReceived}:server_replied={serverReplied}:completed={result.completed}");
        return result;
    }

    private static (long completed, List<double> latencySamples) RunRequestLoop(
        IZlinkSocket progressSocket, int msgSize, int durationSeconds, int latencyCap,
        Func<Message, RequestCallback, bool> submit)
    {
        int payloadSize = Math.Max(msgSize, PerfMetricHeaderSize);
        using var progressPoller = Zlink.CreatePoller();
        var samples = new List<double>(Math.Max(0, latencyCap));
        object gate = new();
        Exception? callbackError = null;
        int hasCallbackError = 0;
        long completed = 0;
        long inFlight = 0;
        long sampleSeen = 0;
        uint rng = 0xA341316Cu;
        ulong seq = 1;
        const int pipelineBudgetBytes = 768 * 1024;
        int maxInFlight = Math.Max(1,
            Math.Min(64, pipelineBudgetBytes / Math.Max(1, msgSize)));
        long deadlineTicks = DeadlineTicksFromSeconds(durationSeconds);

        bool HasCallbackError()
        {
            return Volatile.Read(ref hasCallbackError) != 0;
        }

        void RecordCallbackError(Exception ex)
        {
            lock (gate)
            {
                callbackError ??= ex;
                Volatile.Write(ref hasCallbackError, 1);
            }
        }

        bool TrySubmitOne()
        {
            if (Stopwatch.GetTimestamp() >= deadlineTicks || HasCallbackError())
                return false;

            Interlocked.Increment(ref inFlight);
            int counted = 1;
            using Message message = Message.Allocate(payloadSize);
            long sentTicks = Stopwatch.GetTimestamp();
            StampMetricHeader(message.AsSpan(), RunId, ActivePhase, msgSize,
                seq++, EpochNsFromTimestamp(sentTicks));

            bool submitted = false;
            try
            {
                submitted = submit(message, (result, parts) =>
                {
                    try
                    {
                        if (result != RequestResult.Ok)
                        {
                            DebugLog($"single_reqrep_callback_result:{result}");
                            return;
                        }
                        if (parts.Count <= 0)
                            return;
                        ReadOnlySpan<byte> body = parts[parts.Count - 1]
                            .AsReadOnlySpan();
                        if (!TryDecodeExpectedSingleHeader(body, msgSize,
                                ActivePhase, out var header, RunId))
                        {
                            return;
                        }

                        ulong nowNs = EpochNs();
                        lock (gate)
                        {
                            if (nowNs >= header.SentTsNs)
                            {
                                ReservoirSample(samples!,
                                    nowNs - header.SentTsNs,
                                    ref sampleSeen, latencyCap, ref rng);
                            }
                        }

                        Interlocked.Increment(ref completed);
                    }
                    catch (Exception ex)
                    {
                        RecordCallbackError(ex);
                    }
                    finally
                    {
                        Zlink.MultipartClose(parts);
                        if (Interlocked.Exchange(ref counted, 0) == 1)
                            Interlocked.Decrement(ref inFlight);
                    }
                });
                return submitted;
            }
            catch (ZlinkException ex)
                when (PerfShared.IsTransientBackpressure(ex.NativeErrno)
                      || PerfShared.IsTransientNetworkError(ex.NativeErrno))
            {
                return false;
            }
            finally
            {
                if (!submitted
                    && Interlocked.Exchange(ref counted, 0) == 1)
                {
                    Interlocked.Decrement(ref inFlight);
                }
            }
        }

        progressPoller.Add(progressSocket, PollEventFlags.PollCompletion, 1);
        var events = new PollEvent[1];
        TimeSpan drainTimeout = ResolveReqRepDrainTimeout();
        while (Stopwatch.GetTimestamp() < deadlineTicks && !HasCallbackError())
        {
            bool submittedAny = false;
            int submittedSinceProgress = 0;
            // HOT PATH: keep request count and total payload below the same
            // balanced pipeline budget as C. Removing this bound turns queue
            // residence into the latency metric and times out large payloads.
            while (Volatile.Read(ref inFlight) < maxInFlight
                   && Stopwatch.GetTimestamp() < deadlineTicks
                   && !HasCallbackError())
            {
                if (!TrySubmitOne())
                    break;
                submittedAny = true;
                if (++submittedSinceProgress >= 64)
                {
                    submittedSinceProgress = 0;
                    WaitForCompletion(progressPoller, events, 0);
                }
            }

            if (!submittedAny && Volatile.Read(ref inFlight) == 0)
            {
                Thread.Yield();
                continue;
            }

            WaitForCompletion(progressPoller, events, 50);
        }

        long drainStartTicks = Stopwatch.GetTimestamp();
        while (Volatile.Read(ref inFlight) > 0 && !HasCallbackError())
        {
            WaitForCompletion(progressPoller, events, 50);
            if (Stopwatch.GetElapsedTime(drainStartTicks) > drainTimeout)
            {
                DebugLog(
                    $"single_reqrep_drain_timeout:in_flight={Volatile.Read(ref inFlight)}:completed={Volatile.Read(ref completed)}");
                throw new TimeoutException("request/reply callbacks did not drain");
            }
        }

        lock (gate)
        {
            if (callbackError != null)
                throw callbackError;
        }
        return (Volatile.Read(ref completed), samples);
    }

    private static void WaitForCompletion(IPoller poller,
        Span<PollEvent> events, int timeoutMs)
    {
        // The Windows native completion poller can remain blocked after a
        // completion notification race. Match the C perf workaround: poll
        // without blocking, then yield briefly so the callback progresses.
        if (OperatingSystem.IsWindows())
        {
            _ = poller.Wait(events, TimeSpan.Zero);
            if (timeoutMs > 0)
                Thread.Sleep(1);
            return;
        }

        _ = poller.Wait(events, TimeSpan.FromMilliseconds(timeoutMs));
    }

    private static void RunReplyLoop(IRouterSocket server, ManualResetEventSlim stop,
        ref long serverReceived, ref long serverReplied)
    {
        using var received = Received.Create();
        while (!stop.IsSet)
        {
            if (!TryReceiveBlocking(server, received))
                continue;
            Interlocked.Increment(ref serverReceived);
            if (!TryGetPayloadPart(received, out Message payloadPart))
                continue;
            if (StopToken.IsStopToken(payloadPart.AsReadOnlySpan()))
                return;
            if (!received.RequestSeq.HasValue)
                continue;

            // HOT PATH: the C replier transfers the received native message
            // into the reply. Preserve that ownership transfer instead of
            // adding a binding-only allocation and full-payload copy.
            received.Reply().Message(payloadPart).Submit();
            Interlocked.Increment(ref serverReplied);
        }
    }

    private static TimeSpan ResolveReqRepTimeout()
    {
        int ms = PerfEnv.ReadPositive("PERF_SINGLE_REQREP_TIMEOUT_MS", 200);
        return TimeSpan.FromMilliseconds(ms);
    }

    private static TimeSpan ResolveReqRepDrainTimeout()
    {
        int ms = PerfEnv.ReadPositive("PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS", 10000);
        return TimeSpan.FromMilliseconds(ms);
    }

    private static bool TryReceiveBlocking(IRouterSocket receiver, Received result)
    {
        try
        {
            return receiver.Recv(result);
        }
        catch (ZlinkRecvException ex) when (IsInterrupted(ex.NativeErrno)
                                            || IsWouldBlock(ex.NativeErrno))
        {
            return false;
        }
        catch (ZlinkException ex) when (IsInterrupted(ex.NativeErrno)
                                        || IsWouldBlock(ex.NativeErrno))
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

    private static bool IsPayload(Received received, ReadOnlySpan<byte> expected)
    {
        if (!TryGetPayloadPart(received, out Message payloadPart))
            return false;
        return payloadPart.AsReadOnlySpan().SequenceEqual(expected);
    }

    private static void TryCleanup(IConnectableSocket client, ISocket server,
        string endpoint, string tag)
    {
        try
        {
            client.Disconnect(endpoint);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"{tag} cleanup disconnect failed: {ex.Message}");
        }

        try
        {
            server.Unbind(endpoint);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"{tag} cleanup unbind failed: {ex.Message}");
        }
    }

    private static bool IsInterrupted(int errno)
    {
        return PerfShared.IsInterrupted(errno);
    }

    private static bool IsWouldBlock(int errno)
    {
        return PerfShared.IsWouldBlock(errno);
    }
}
