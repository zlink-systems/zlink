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

    internal static async Task<int> RunDealerRouter(string transport, int size)
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

            var result = await RunDealerRouterActiveAsync(client, server, size,
                durationSeconds, latencySampleCap).ConfigureAwait(false);
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

    internal static async Task<int> RunRouterRouter(string transport, int size)
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

            RoutingId? targetRoutingId = await CompleteRouterRouterHandshakeAsync(
                client, server, readyTimeoutMs).ConfigureAwait(false);
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

            var result = await RunRouterRouterActiveAsync(client, server,
                targetRoutingId.Value, size, durationSeconds, latencySampleCap)
                .ConfigureAwait(false);
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

    private static async Task<(long completed, List<double> latencySamples)>
        RunDealerRouterActiveAsync(
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

        var result = await RunRequestLoopAsync(
            msgSize,
            durationSeconds,
            latencyCap,
            async message =>
            {
                using Message? tail = PerfSocketIo.MeasurementPartCount == 2
                    ? Message.Allocate(0) : null;
                return tail == null
                    ? await client.Request().Message(message)
                        .Timeout(ResolveReqRepTimeout()).Async().ConfigureAwait(false)
                    : await client.Request().Message(message).Message(tail)
                        .Timeout(ResolveReqRepTimeout()).Async().ConfigureAwait(false);
            }).ConfigureAwait(false);
        stop.Set();
        if (!await SendStopTokenAsync(client,
                "[single-dealer-router-reqrep]").ConfigureAwait(false))
            throw new TimeoutException("dealer-router reqrep stop token was not sent");
        serverThread.Join();
        if (serverError != null)
            throw serverError;
        DebugLog(
            $"single_dealer_router_reqrep_debug:server_received={serverReceived}:server_replied={serverReplied}:completed={result.completed}");
        return result;
    }

    private static async Task<RoutingId?> CompleteRouterRouterHandshakeAsync(
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
            try
            {
                using Message pingMessage = Message.From(ping);
                await client.Send(RouterReqRepServerRid)
                    .Message(pingMessage)
                    .Async().ConfigureAwait(false);
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
            return null;

        try
        {
            using Message pongMessage = Message.From(pong);
            await server.Send(clientActualRoutingId.Value)
                .Message(pongMessage)
                .Async().ConfigureAwait(false);
        }
        catch (ZlinkException ex)
            when (PerfShared.IsTransientBackpressure(ex.NativeErrno)
                  || PerfShared.IsTransientNetworkError(ex.NativeErrno))
        {
            return null;
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

    private static async Task<(long completed, List<double> latencySamples)>
        RunRouterRouterActiveAsync(
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

        var result = await RunRequestLoopAsync(
            msgSize,
            durationSeconds,
            latencyCap,
            async message =>
            {
                using Message? tail = PerfSocketIo.MeasurementPartCount == 2
                    ? Message.Allocate(0) : null;
                return tail == null
                    ? await client.Request(targetRid).Message(message)
                        .Timeout(ResolveReqRepTimeout()).Async().ConfigureAwait(false)
                    : await client.Request(targetRid).Message(message).Message(tail)
                        .Timeout(ResolveReqRepTimeout()).Async().ConfigureAwait(false);
            }).ConfigureAwait(false);
        stop.Set();
        if (!await SendRoutedStopTokenAsync(client, targetRid,
                "[single-router-router-reqrep]").ConfigureAwait(false))
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

    private static async Task<(long completed, List<double> latencySamples)>
        RunRequestLoopAsync(
        int msgSize, int durationSeconds, int latencyCap,
        Func<Message, Task<IReadOnlyList<Message>>> submit)
    {
        int payloadSize = Math.Max(msgSize, PerfMetricHeaderSize);
        using var completionSignal = new SemaphoreSlim(0);
        var samples = new List<double>(Math.Max(0, latencyCap));
        object gate = new();
        Exception? completionError = null;
        int hasCompletionError = 0;
        long completed = 0;
        long inFlight = 0;
        long sampleSeen = 0;
        uint rng = 0xA341316Cu;
        ulong seq = 1;
        const int pipelineBudgetBytes = 768 * 1024;
        int maxInFlight = Math.Max(1,
            Math.Min(64, pipelineBudgetBytes / Math.Max(1, msgSize)));
        long deadlineTicks = DeadlineTicksFromSeconds(durationSeconds);

        bool HasCompletionError()
        {
            return Volatile.Read(ref hasCompletionError) != 0;
        }

        void RecordCompletionError(Exception ex)
        {
            lock (gate)
            {
                completionError ??= ex;
                Volatile.Write(ref hasCompletionError, 1);
            }
        }

        bool TrySubmitOne()
        {
            if (Stopwatch.GetTimestamp() >= deadlineTicks || HasCompletionError())
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
                Task<IReadOnlyList<Message>> request = submit(message);
                submitted = true;
                _ = ObserveRequestAsync(request);
                return true;
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

            async Task ObserveRequestAsync(Task<IReadOnlyList<Message>> request)
            {
                IReadOnlyList<Message>? parts = null;
                try
                {
                    parts = await request.ConfigureAwait(false);
                    if (!PerfSocketIo.TryMeasurementPayload(parts,
                            out Message replyPayload)
                        || !TryDecodeExpectedSingleHeader(
                            replyPayload.AsReadOnlySpan(), msgSize,
                            ActivePhase, out var header, RunId))
                    {
                        return;
                    }

                    ulong nowNs = EpochNs();
                    lock (gate)
                    {
                        if (nowNs >= header.SentTsNs)
                        {
                            ReservoirSample(samples!, nowNs - header.SentTsNs,
                                ref sampleSeen, latencyCap, ref rng);
                        }
                    }
                    Interlocked.Increment(ref completed);
                }
                catch (ZlinkRequestException ex)
                    when (ex.Result == ZlinkRequestException.ErrorCode.TimedOut)
                {
                    DebugLog("single_reqrep_completion_result:timed_out");
                }
                catch (Exception ex)
                {
                    RecordCompletionError(ex);
                }
                finally
                {
                    if (parts != null)
                        Zlink.MultipartClose(parts);
                    if (Interlocked.Exchange(ref counted, 0) == 1)
                        Interlocked.Decrement(ref inFlight);
                    completionSignal.Release();
                }
            }
        }

        TimeSpan drainTimeout = ResolveReqRepDrainTimeout();
        while (Stopwatch.GetTimestamp() < deadlineTicks && !HasCompletionError())
        {
            bool submittedAny = false;
            int submittedSinceProgress = 0;
            // HOT PATH: keep request count and total payload below the same
            // balanced pipeline budget as C. Removing this bound turns queue
            // residence into the latency metric and times out large payloads.
            while (Volatile.Read(ref inFlight) < maxInFlight
                   && Stopwatch.GetTimestamp() < deadlineTicks
                   && !HasCompletionError())
            {
                if (!TrySubmitOne())
                    break;
                submittedAny = true;
                if (++submittedSinceProgress >= 64)
                {
                    submittedSinceProgress = 0;
                    _ = await completionSignal.WaitAsync(0)
                        .ConfigureAwait(false);
                }
            }

            if (!submittedAny && Volatile.Read(ref inFlight) == 0)
            {
                Thread.Yield();
                continue;
            }

            _ = await completionSignal.WaitAsync(50).ConfigureAwait(false);
        }

        long drainStartTicks = Stopwatch.GetTimestamp();
        while (Volatile.Read(ref inFlight) > 0 && !HasCompletionError())
        {
            _ = await completionSignal.WaitAsync(50).ConfigureAwait(false);
            if (Stopwatch.GetElapsedTime(drainStartTicks) > drainTimeout)
            {
                DebugLog(
                    $"single_reqrep_drain_timeout:in_flight={Volatile.Read(ref inFlight)}:completed={Volatile.Read(ref completed)}");
                throw new TimeoutException("request/reply operations did not drain");
            }
        }

        lock (gate)
        {
            if (completionError != null)
                throw completionError;
        }
        return (Volatile.Read(ref completed), samples);
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
            if (received.Parts.Count == 1
                && StopToken.IsStopToken(received.FirstPart().AsReadOnlySpan()))
                return;
            if (!PerfSocketIo.TryMeasurementPayload(received.Parts,
                    out Message payloadPart))
                continue;
            if (!received.RequestSeq.HasValue)
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
