using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfRouterRouter
{
    private static readonly RoutingId ReceiverRoutingId =
        RoutingId.From("ROUTER1"u8);
    private static readonly RoutingId SenderRoutingId =
        RoutingId.From("ROUTER2"u8);
    private const uint RunId = 1;
    private const uint ReadyPhase = 0;
    private const uint ActivePhase = 1;

    private static void TryCleanup(IRouterSocket sender, IRouterSocket receiver,
        string endpoint)
    {
        try
        {
            sender.Disconnect(endpoint);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[single-router-router] cleanup disconnect failed: {ex.Message}");
        }

        try
        {
            receiver.Unbind(endpoint);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[single-router-router] cleanup unbind failed: {ex.Message}");
        }
    }

    internal static async Task<int> RunRouterRouter(string transport, int size)
    {
        int durationSeconds = ResolveSingleDurationSeconds();
        int recvTimeoutMs = ResolveSingleRcvTimeoutMs();
        int readyTimeoutMs = ResolveSingleConnectReadyTimeoutMs();
        int latencySampleCap = ResolveSingleLatencyCount("ROUTER_ROUTER");

        using var ctx = Zlink.CreateContext();
        ApplySingleContextOptions(ctx);
        using var receiver = ctx.CreateRouterSocket();
        using var sender = ctx.CreateRouterSocket();
        ApplySingleSocketOptions(receiver);
        ApplySingleSocketOptions(sender);
        RecalculateSingleAutoHwm(ctx);
        ConfigureTlsServerIfNeeded(receiver, transport);
        ConfigureTlsClientIfNeeded(sender, transport);
        MonitorSocket? receiverMonitor = null;
        MonitorSocket? senderMonitor = null;
        string endpoint = EndpointFor(transport, "router-router");

        try
        {
            receiverMonitor = receiver.MonitorOpen(SocketEvent.ConnectionReady);
            senderMonitor = sender.MonitorOpen(SocketEvent.ConnectionReady);

            receiver.SetRoutingId(ReceiverRoutingId);
            sender.SetRoutingId(SenderRoutingId);
            receiver.Options.Mandatory = true;
            sender.Options.Mandatory = true;
            receiver.Bind(endpoint);
            endpoint = receiver.Options.LastEndpoint;
            sender.Connect(endpoint);
            if (!(WaitForConnectionReadyWithActivity(receiverMonitor, receiver,
                    readyTimeoutMs, acceptAccepted: false)
                && WaitForConnectionReadyWithActivity(senderMonitor, sender,
                    readyTimeoutMs, acceptAccepted: false)))
            {
                DebugLog("single_router_router_error:connection_not_ready");
                TryCleanup(sender, receiver, endpoint);
                return 2;
            }

            var handshake = await CompleteHandshakeAsync(sender, receiver,
                readyTimeoutMs).ConfigureAwait(false);
            if (!handshake.Ok)
            {
                TryCleanup(sender, receiver, endpoint);
                DebugLog("single_router_router_error:route_probe_failed");
                return 2;
            }

            // ITEM 1: capture AUTO_HWM_DETAIL from the live monitors BEFORE
            // they are disposed (C parity; auto-HWM applied values are stable
            // once configured and connection-ready).
            EmitSingleAutoHwmDetail(receiverMonitor, "ROUTER_ROUTER",
                transport, "receiver", "router", size);
            EmitSingleAutoHwmDetail(senderMonitor, "ROUTER_ROUTER",
                transport, "sender", "router", size);

            receiverMonitor.Dispose();
            receiverMonitor = null;
            senderMonitor.Dispose();
            senderMonitor = null;

            int payloadSize = Math.Max(size, PerfMetricHeaderSize);
            var payload = new byte[payloadSize];
            Array.Fill(payload, (byte)'a');

            var active = await RunActivePhaseAsync(sender, receiver,
                handshake.TargetRoutingId, payload, size, durationSeconds,
                recvTimeoutMs, latencySampleCap).ConfigureAwait(false);
            if (!active.Ok)
            {
                DebugLog("single_router_router_error:active_failed");
                TryCleanup(sender, receiver, endpoint);
                return 2;
            }

            double throughput = active.Received
                / (double)Math.Max(durationSeconds, 1);
            var latency = ComputeLatencyStats(active.LatencySamples);
            PrintResult("ROUTER_ROUTER", transport, size, throughput,
                latency.mean, latency.p95, latency.p99);
            TryCleanup(sender, receiver, endpoint);
            return 0;
        }
        catch (Exception ex)
        {
            TryCleanup(sender, receiver, endpoint);
            DebugLog($"single_router_router_error:exception:{ex}");
            return 2;
        }
        finally
        {
            receiverMonitor?.Dispose();
            senderMonitor?.Dispose();
        }
    }

    private static async Task<(bool Ok, RoutingId TargetRoutingId)>
        CompleteHandshakeAsync(IRouterSocket sender, IRouterSocket receiver,
            int timeoutMs)
    {
        byte[] ping = "PING"u8.ToArray();
        byte[] pong = "PONG"u8.ToArray();
        RoutingId? senderActualRoutingId = null;

        using var poller = Zlink.CreatePoller();
        var events = new PollEvent[1];
        poller.Add(receiver, PollEventFlags.PollIn, 0);
        long deadlineTicks = DeadlineTicksFromMilliseconds(Math.Max(1000, timeoutMs));
        while (senderActualRoutingId == null
               && Stopwatch.GetTimestamp() < deadlineTicks)
        {
            try
            {
                _ = await PerfSocketIo.SendAsync(sender, ReceiverRoutingId,
                    ping, SendFlags.DontWait).ConfigureAwait(false);
            }
            catch (ZlinkException ex)
                when (PerfShared.IsTransientBackpressure(ex.NativeErrno)
                      || IsTransientNetworkError(ex.NativeErrno))
            {
            }

            int waitMs = Math.Max(1,
                (int)Math.Ceiling((deadlineTicks - Stopwatch.GetTimestamp())
                    * 1000.0 / Stopwatch.Frequency));
            if (!WaitForInput(poller, events, waitMs))
                continue;

            using var receivedBuffer = Received.Create();
            while (TryReceive(receiver, receivedBuffer))
            {
                if (receivedBuffer.RoutingId == null
                    || !IsPayload(receivedBuffer, ping))
                    continue;

                senderActualRoutingId = receivedBuffer.RoutingId;
                break;
            }
        }

        if (senderActualRoutingId == null)
            return (false, ReceiverRoutingId);

        try
        {
            if (await PerfSocketIo.SendAsync(receiver,
                    senderActualRoutingId.Value, pong, SendFlags.None)
                    .ConfigureAwait(false) == 0)
                return (false, ReceiverRoutingId);
        }
        catch (ZlinkException ex)
            when (PerfShared.IsTransientBackpressure(ex.NativeErrno)
                  || IsTransientNetworkError(ex.NativeErrno))
        {
            return (false, ReceiverRoutingId);
        }

        using var senderPoller = Zlink.CreatePoller();
        var senderEvents = new PollEvent[1];
        senderPoller.Add(sender, PollEventFlags.PollIn, 0);
        while (Stopwatch.GetTimestamp() < deadlineTicks)
        {
            int waitMs = Math.Max(1,
                (int)Math.Ceiling((deadlineTicks - Stopwatch.GetTimestamp())
                    * 1000.0 / Stopwatch.Frequency));
            if (!WaitForInput(senderPoller, senderEvents, waitMs))
                continue;

            using var response = Received.Create();
            while (TryReceive(sender, response))
            {
                if (response.RoutingId == null || !IsPayload(response, pong))
                    continue;

                return (true, response.RoutingId.Value);
            }
        }

        return (false, ReceiverRoutingId);
    }

    private static async Task<(bool Ok, long Received,
        List<double> LatencySamples)> RunActivePhaseAsync(IRouterSocket sender,
        IRouterSocket receiver,
        RoutingId targetRoutingId, byte[] payload, int msgSize,
        int durationSeconds, int recvTimeoutMs, int latencyCap)
    {
        _ = recvTimeoutMs;
        long received = 0;
        long activeDeadlineTicks = DeadlineTicksFromSeconds(durationSeconds);
        Exception? sendError = null;
        var samples = new List<double>(Math.Max(0, latencyCap));
        long sampleSeen = 0;
        uint rng = 0xA341316Cu;
        bool stopReceived = false;

        bool ProcessReceived(Received receivedMessage)
        {
            if (!TryGetPayloadPart(receivedMessage, out Message payloadMessage))
                return false;

            ReadOnlySpan<byte> body = payloadMessage.AsReadOnlySpan();
            if (StopToken.IsStopToken(body))
                return true;

            if (!TryDecodeExpectedSingleHeader(body, msgSize, ActivePhase,
                    out var header, RunId))
            {
                return false;
            }

            if (Stopwatch.GetTimestamp() < activeDeadlineTicks)
            {
                received++;
                ulong nowNs = EpochNs();
                if (nowNs >= header.SentTsNs)
                {
                    double latencyNs = nowNs - header.SentTsNs;
                    ReservoirSample(samples, latencyNs, ref sampleSeen, latencyCap,
                        ref rng);
                }
            }

            return false;
        }

        var receivedBuffer = Received.Create();

        // PERF_SINGLE_TEST_POLICY § 1.4: sender drops `senderDone` flag.
        // After active deadline it emits the wire-level stop token; the
        // receiver loop exits when it sees the token.
        async Task SendLoopAsync()
        {
            try
            {
                ulong seq = 1;
                long deadlineTicks = DeadlineTicksFromSeconds(durationSeconds);
                while (true)
                {
                    long nowTicks = Stopwatch.GetTimestamp();
                    if (nowTicks >= deadlineTicks)
                        break;
                    // HOT PATH: C stamps and copies every byte of its reusable
                    // payload into each native message. Keep page and cache
                    // work on the same sender path for binding comparison.
                    StampMetricHeader(payload.AsSpan(), RunId, ActivePhase,
                        msgSize, seq, EpochNsFromTimestamp(nowTicks));
                    seq++;
                    try
                    {
                        if (await PerfSocketIo.SendAsync(sender,
                                targetRoutingId, payload, SendFlags.None)
                                .ConfigureAwait(false) == 0)
                            continue;
                    }
                    catch (ZlinkException ex)
                        when (PerfShared.IsTransientBackpressure(
                                  ex.NativeErrno)
                              || IsTransientNetworkError(ex.NativeErrno))
                    {
                        continue;
                    }
                }
            }
            catch (ZlinkException ex) when (IsInterrupted(ex.NativeErrno))
            {
                sendError = ex;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[single-router-router] send failed: {ex.Message}");
                sendError = ex;
            }
            finally
            {
                // Always emit the stop token so the receiver loop exits
                // even on send failures during the active phase.
                if (!await SendRoutedStopTokenAsync(sender, targetRoutingId,
                        "[single-router-router]").ConfigureAwait(false))
                {
                    sendError ??= new TimeoutException(
                        "router-router stop token was not sent");
                }
            }
        }

        Task senderTask = SendLoopAsync();

        // PERF_SINGLE_TEST_POLICY § 1.4: blocking first recv per cycle, then
        // DontWait burst-drain. The phase ends purely when the wire-level stop
        // token arrives, matching C perf_router_router.cpp run_active_phase.
        while (!stopReceived)
        {
            if (!TryReceiveBlocking(receiver, receivedBuffer))
                continue;

            bool drain = true;
            while (drain)
            {
                if (ProcessReceived(receivedBuffer))
                {
                    stopReceived = true;
                    break;
                }

                drain = TryReceive(receiver, receivedBuffer);
            }
        }

        await senderTask.ConfigureAwait(false);

        if (sendError != null)
        {
            DebugLog("single_router_router_error:send_failed");
            return (false, received, samples);
        }

        return (received > 0 && samples.Count > 0, received, samples);
    }

    private static bool TryReceive(IRouterSocket receiver, Received result)
    {
        try
        {
            return receiver.Recv(result, RecvFlags.DontWait);
        }
        catch (ZlinkRecvException)
        {
            return false;
        }
        catch (ZlinkException ex) when (IsInterrupted(ex.NativeErrno)
                                        || IsWouldBlock(ex.NativeErrno))
        {
            return false;
        }
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

        if (received.RoutingId != null && received.Parts.Count > 0)
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

    private static bool IsInterrupted(int errno)
    {
        return PerfShared.IsInterrupted(errno);
    }

    private static bool IsWouldBlock(int errno)
    {
        return PerfShared.IsWouldBlock(errno);
    }

    private static bool IsTransientNetworkError(int errno)
    {
        return PerfShared.IsTransientNetworkError(errno);
    }
}
