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
            if (!(WaitForConnectionReadyWithActivity(serverMonitor, server,
                    readyTimeoutMs, acceptAccepted: false)
                && WaitForConnectionReadyWithActivity(clientMonitor, client,
                    readyTimeoutMs, acceptAccepted: false)))
            {
                DebugLog("single_router_router_reqrep_error:connection_not_ready");
                TryCleanup(client, server, endpoint, "[single-router-router-reqrep]");
                return 2;
            }

            RoutingId? targetRoutingId = CompleteRouterRouterHandshake(
                client, server, readyTimeoutMs);
            if (!targetRoutingId.HasValue)
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
                targetRoutingId.Value, size, durationSeconds, latencySampleCap);
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

    private static (long completed, List<double> latencySamples)
        RunDealerRouterActive(
        IDealerSocket client, IRouterSocket server, int msgSize,
        int durationSeconds, int latencyCap)
    {
        Exception? serverError = null;
        long serverReceived = 0;
        long serverReplied = 0;
        var serverThread = new Thread(() =>
        {
            try
            {
                RunReplyLoop(server, ref serverReceived, ref serverReplied);
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

        (long completed, List<double> latencySamples) result = (0, new());
        Exception? requestError = null;
        try
        {
            result = RunRequestLoop(
                msgSize,
                durationSeconds,
                latencyCap,
                message =>
                {
                    using Message? tail = PerfSocketIo.MeasurementPartCount == 2
                        ? Message.Allocate(0) : null;
                    if (tail == null)
                        return client.Request().Message(message)
                            .Timeout(ResolveReqRepTimeout())
                            .Async();
                    return client.Request().Message(message).Message(tail)
                        .Timeout(ResolveReqRepTimeout())
                        .Async();
                });
        }
        catch (Exception ex)
        {
            requestError = ex;
        }
        finally
        {
            try
            {
                using Message stop = Message.From(StopToken.Bytes);
                client.Send().Message(stop).Async().Admitted.GetAwaiter()
                    .GetResult();
            }
            catch (Exception ex)
            {
                requestError ??= ex;
            }
            serverThread.Join();
        }
        if (requestError != null)
            throw requestError;
        if (serverError != null)
            throw serverError;
        DebugLog(
            $"single_dealer_router_reqrep_debug:server_received={serverReceived}:server_replied={serverReplied}:completed={result.completed}");
        return result;
    }

    private static RoutingId? CompleteRouterRouterHandshake(
        IRouterSocket client, IRouterSocket server, int timeoutMs)
    {
        byte[] ping = "PING"u8.ToArray();
        byte[] pong = "PONG"u8.ToArray();
        RoutingId? clientActualRoutingId = null;

        using var poller = Zlink.CreatePoller();
        var events = new PollEvent[1];
        poller.Add(server, PollEventFlags.PollIn, 0);
        long deadlineTicks =
            DeadlineTicksFromMilliseconds(Math.Max(1000, timeoutMs));
        while (clientActualRoutingId == null
               && Stopwatch.GetTimestamp() < deadlineTicks)
        {
            using Message pingMessage = Message.From(ping);
            client.Send(RouterReqRepServerRid)
                .Message(pingMessage)
                .Async().Admitted.GetAwaiter().GetResult();

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
            return null;

        using (Message pongMessage = Message.From(pong))
        {
            server.Send(clientActualRoutingId.Value)
                .Message(pongMessage)
                .Async().Admitted.GetAwaiter().GetResult();
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

                return response.RoutingId.Value;
            }
        }

        return null;
    }

    private static (long completed, List<double> latencySamples)
        RunRouterRouterActive(
        IRouterSocket client, IRouterSocket server, RoutingId targetRid,
        int msgSize, int durationSeconds, int latencyCap)
    {
        Exception? serverError = null;
        long serverReceived = 0;
        long serverReplied = 0;
        var serverThread = new Thread(() =>
        {
            try
            {
                RunReplyLoop(server, ref serverReceived, ref serverReplied);
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

        (long completed, List<double> latencySamples) result = (0, new());
        Exception? requestError = null;
        try
        {
            result = RunRequestLoop(
                msgSize,
                durationSeconds,
                latencyCap,
                message =>
                {
                    using Message? tail = PerfSocketIo.MeasurementPartCount == 2
                        ? Message.Allocate(0) : null;
                    if (tail == null)
                        return client.Request(targetRid).Message(message)
                            .Timeout(ResolveReqRepTimeout())
                            .Async();
                    return client.Request(targetRid).Message(message).Message(tail)
                        .Timeout(ResolveReqRepTimeout())
                        .Async();
                });
        }
        catch (Exception ex)
        {
            requestError = ex;
        }
        finally
        {
            if (!SendRoutedStopTokenBlocking(client, targetRid,
                    "[single-router-router-reqrep]"))
            {
                requestError ??= new TimeoutException(
                    "router-router reqrep stop token was not sent");
            }
            serverThread.Join();
        }
        if (requestError != null)
            throw requestError;
        if (serverError != null)
            throw serverError;
        DebugLog(
            $"single_router_router_reqrep_debug:server_received={serverReceived}:server_replied={serverReplied}:completed={result.completed}");
        return result;
    }

    private static (long completed, List<double> latencySamples) RunRequestLoop(
        int msgSize, int durationSeconds, int latencyCap,
        Func<Message, RequestSubmission> submit)
    {
        int payloadSize = Math.Max(msgSize, PerfMetricHeaderSize);
        var samples = new List<double>(Math.Max(0, latencyCap));
        var replySignal = new SemaphoreSlim(0);
        object gate = new();
        Exception? completionError = null;
        int fatal = 0;
        long pendingReplies = 0;
        long completed = 0;
        long sampleSeen = 0;
        uint rng = 0xA341316Cu;

        void RecordError(Exception ex)
        {
            lock (gate)
                completionError ??= ex;
            Volatile.Write(ref fatal, 1);
        }

        var requesterThread = new Thread(() =>
        {
            try
            {
                ulong seq = 1;
                long deadlineTicks = DeadlineTicksFromSeconds(durationSeconds);

                async Task ObserveReplyAsync(
                    Task<IReadOnlyList<Message>> reply)
                {
                    IReadOnlyList<Message>? parts = null;
                    try
                    {
                        parts = await reply.ConfigureAwait(false);
                        if (!PerfSocketIo.TryMeasurementPayload(parts,
                                out Message replyPayload))
                        {
                            throw new InvalidOperationException(
                                "request reply has an invalid measurement shape");
                        }
                        if (!TryDecodeExpectedSingleHeader(
                                replyPayload.AsReadOnlySpan(), msgSize,
                                ActivePhase, out var header, RunId))
                        {
                            return;
                        }
                        long completionTicks = Stopwatch.GetTimestamp();
                        if (completionTicks >= deadlineTicks)
                            return;
                        ulong nowNs = EpochNsFromTimestamp(completionTicks);
                        if (nowNs < header.SentTsNs)
                            return;
                        lock (gate)
                        {
                            ReservoirSample(samples!, nowNs - header.SentTsNs,
                                ref sampleSeen, latencyCap, ref rng);
                            completed++;
                        }
                    }
                    catch (ZlinkRequestException ex)
                        when (ex.Result == ZlinkRequestException.ErrorCode.TimedOut)
                    {
                    }
                    catch (Exception ex)
                    {
                        RecordError(ex);
                    }
                    finally
                    {
                        if (parts != null)
                            Zlink.MultipartClose(parts);
                        Interlocked.Decrement(ref pendingReplies);
                        replySignal.Release();
                    }
                }

                while (Stopwatch.GetTimestamp() < deadlineTicks
                       && Volatile.Read(ref fatal) == 0)
                {
                    using (Message message = Message.Allocate(payloadSize))
                    {
                        long sentTicks = Stopwatch.GetTimestamp();
                        StampMetricHeader(message.AsSpan(), RunId, ActivePhase,
                            msgSize, seq, EpochNsFromTimestamp(sentTicks));
                        try
                        {
                            RequestSubmission submission = submit(message);
                            Interlocked.Increment(ref pendingReplies);
                            _ = ObserveReplyAsync(submission.Reply);
                            seq++;
                            if (submission.Result ==
                                SubmitResult.Backpressured)
                            {
                                submission.Admitted.GetAwaiter().GetResult();
                            }
                        }
                        catch (Exception ex)
                        {
                            RecordError(ex);
                        }
                    }
                }

                long drainDeadline = DeadlineTicksFromMilliseconds(
                    ResolveReqRepDrainTimeoutMs());
                while (Volatile.Read(ref pendingReplies) > 0)
                {
                    long remainingTicks = drainDeadline
                        - Stopwatch.GetTimestamp();
                    if (remainingTicks <= 0
                        || !replySignal.Wait(TimeSpan.FromSeconds(
                            remainingTicks / (double)Stopwatch.Frequency)))
                    {
                        RecordError(new TimeoutException(
                            "request/reply operations did not drain"));
                        break;
                    }
                }
            }
            catch (Exception ex)
            {
                RecordError(ex);
            }
        })
        {
            IsBackground = true,
            Name = "single reqrep requester"
        };

        requesterThread.Start();
        requesterThread.Join();

        lock (gate)
        {
            if (completionError != null)
                throw completionError;
            return (completed, samples);
        }
    }

    private static void RunReplyLoop(IRouterSocket server,
        ref long serverReceived, ref long serverReplied)
    {
        using var received = Received.Create();
        while (true)
        {
            if (!TryReceiveBlocking(server, received))
                continue;
            Interlocked.Increment(ref serverReceived);
            if (received.Parts.Count == 1
                && StopToken.IsStopToken(received.FirstPart().AsReadOnlySpan()))
                return;
            if (!PerfSocketIo.TryMeasurementPayload(received.Parts,
                    out Message payloadPart))
                continue;
            if (received.ReplyToken == null)
                continue;

            // HOT PATH: the C replier transfers the received native message
            // into the reply. Preserve that ownership transfer instead of
            // adding a binding-only allocation and full-payload copy.
            using (Message tail = Message.Allocate(0))
            {
                if (PerfSocketIo.MeasurementPartCount == 2)
                    received.Reply().Message(payloadPart).Message(tail).Submit();
                else
                    received.Reply().Message(payloadPart).Submit();
            }
            Interlocked.Increment(ref serverReplied);
        }
    }

    // Read once per process: the runner fixes this launch-time knob before
    // starting the benchmark, and the request submit lambda calls this for every
    // submitted request. A per-call GetEnvironmentVariable would put harness
    // instrumentation inside the measured path and charge it only to the
    // binding runner. C reference:
    // bindings/c/perf/common/perf_zlink_part_helpers.hpp
    // perf_measurement_part_count.
    private static readonly TimeSpan ReqRepTimeout = TimeSpan.FromMilliseconds(
        PerfEnv.ReadPositive("PERF_SINGLE_REQREP_TIMEOUT_MS", 200));

    private static TimeSpan ResolveReqRepTimeout()
    {
        return ReqRepTimeout;
    }

    private static int ResolveReqRepDrainTimeoutMs()
    {
        return PerfEnv.ReadPositive("PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS", 10000);
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
