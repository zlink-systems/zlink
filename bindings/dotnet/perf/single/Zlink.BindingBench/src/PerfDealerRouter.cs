using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfDealerRouter
{
    private const uint RunId = 1;
    private const uint ActivePhase = 1;

    private static bool TryCleanup(IDealerSocket sender, IRouterSocket receiver,
        string endpoint)
    {
        bool ok = true;
        try
        {
            sender.Disconnect(endpoint);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[single-dealer-router] cleanup disconnect failed: {ex.Message}");
            ok = false;
        }

        try
        {
            receiver.Unbind(endpoint);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[single-dealer-router] cleanup unbind failed: {ex.Message}");
            ok = false;
        }

        return ok;
    }

    internal static int RunDealerRouter(string transport, int size)
    {
        int durationSeconds = ResolveSingleDurationSeconds();
        int recvTimeoutMs = ResolveSingleRcvTimeoutMs();
        int readyTimeoutMs = ResolveSingleConnectReadyTimeoutMs();
        int latencySampleCap = ResolveSingleLatencyCount("DEALER_ROUTER");

        using var ctx = Zlink.CreateContext();
        ApplySingleContextOptions(ctx);
        using var receiver = ctx.CreateRouterSocket();
        using var sender = ctx.CreateDealerSocket();
        ApplySingleAutoHwmMsgUnit(ctx, size);
        RecalculateSingleAutoHwm(ctx);
        ConfigureTlsServerIfNeeded(receiver, transport);
        ConfigureTlsClientIfNeeded(sender, transport);
        MonitorSocket? receiverMonitor = null;
        MonitorSocket? senderMonitor = null;
        string endpoint = EndpointFor(transport, "dealer-router");

        try
        {
            receiver.Bind(endpoint);
            endpoint = receiver.Options.LastEndpoint;
            receiverMonitor = receiver.MonitorOpen(SocketEvent.ConnectionReady);
            senderMonitor = sender.MonitorOpen(SocketEvent.ConnectionReady);
            sender.Connect(endpoint);
            ApplySingleSocketOptions(receiver);
            ApplySingleSocketOptions(sender);
            if (!(WaitForConnectionReady(receiverMonitor, readyTimeoutMs)
                && WaitForConnectionReady(senderMonitor, readyTimeoutMs)))
            {
                DebugLog("single_dealer_router_error:connection_not_ready");
                TryCleanup(sender, receiver, endpoint);
                return 2;
            }

            // PERF_SINGLE_TEST_POLICY § 1.4 / C perf_dealer_router.cpp
            // run_dealer_router: go directly from the CONNECTION_READY gate
            // to the active phase. C defines wait_for_dealer_router_ready but
            // never calls it (only the ROUTER_ROUTER pattern performs a
            // routing self-check / PING-PONG handshake), so no pre-active
            // routing probe is performed here. This keeps the active-start
            // anchor identical to C.
            // ITEM 1: capture AUTO_HWM_DETAIL from the live monitors BEFORE
            // they are disposed (C parity; auto-HWM applied values are stable
            // once configured and connection-ready).
            EmitSingleAutoHwmDetail(receiverMonitor, "DEALER_ROUTER",
                transport, "receiver", "router", size);
            EmitSingleAutoHwmDetail(senderMonitor, "DEALER_ROUTER",
                transport, "sender", "dealer", size);

            receiverMonitor.Dispose();
            receiverMonitor = null;
            senderMonitor.Dispose();
            senderMonitor = null;

            int payloadSize = Math.Max(size, PerfMetricHeaderSize);
            var payload = new byte[payloadSize];
            Array.Fill(payload, (byte)'a');

            if (!RunActivePhase(sender, receiver, payload, size, durationSeconds,
                    recvTimeoutMs, latencySampleCap, out long received,
                    out var latencySamples))
            {
                DebugLog("single_dealer_router_error:active_failed");
                TryCleanup(sender, receiver, endpoint);
                return 2;
            }

            double throughput = received / (double)Math.Max(durationSeconds, 1);
            var latency = ComputeLatencyStats(latencySamples);
            PrintResult("DEALER_ROUTER", transport, size, throughput,
                latency.mean, latency.p95, latency.p99);
            TryCleanup(sender, receiver, endpoint);
            return 0;
        }
        catch (Exception ex)
        {
            TryCleanup(sender, receiver, endpoint);
            DebugLog($"single_dealer_router_error:exception:{ex}");
            return 2;
        }
        finally
        {
            receiverMonitor?.Dispose();
            senderMonitor?.Dispose();
        }
    }

    private static bool RunActivePhase(IDealerSocket sender, IRouterSocket receiver,
        byte[] payload, int msgSize, int durationSeconds, int recvTimeoutMs,
        int latencyCap, out long receivedOut, out List<double> latencySamples)
    {
        _ = recvTimeoutMs;
        long received = 0;
        Exception? sendError = null;
        var samples = new List<double>(Math.Max(0, latencyCap));
        long sampleSeen = 0;
        uint rng = 0xA341316Cu;
        ulong seq = 1;
        bool stopReceived = false;

        // PERF_SINGLE_TEST_POLICY § 1.4 / C parity: active phase ends only
        // when the receiver observes the wire-level stop token. Active sends
        // use blocking send with transient backpressure retry until the
        // deadline; no binding-local in-flight cap is applied. ROUTER recv is
        // multi-part ([routing-id..., payload]); the payload is the last part
        // (TryGetPayloadPart). Latency stays recv_now_ns - sent_ts_ns.
        bool ProcessBody(ReadOnlySpan<byte> body)
        {
            if (StopToken.IsStopToken(body))
                return true;
            if (TryDecodeExpectedSingleHeader(body, msgSize, ActivePhase,
                    out var header, RunId))
            {
                received++;
                ulong nowNs = EpochNs();
                if (nowNs >= header.SentTsNs)
                {
                    double latencyNs = nowNs - header.SentTsNs;
                    ReservoirSample(samples, latencyNs, ref sampleSeen,
                        latencyCap, ref rng);
                }
            }
            return false;
        }

        using var poller = Zlink.CreatePoller();
        var events = new PollEvent[1];
        poller.Add(receiver, PollEventFlags.PollIn, 0);
        using var maybe = Received.Create();

        var senderThread = new Thread(() =>
        {
            try
            {
                // C starts its active deadline inside the sender thread. Keep
                // setup time outside the measured interval and start only
                // after the receiver's poll registration is ready.
                long deadlineTicks = DeadlineTicksFromSeconds(durationSeconds);
                while (true)
                {
                    long nowTicks = Stopwatch.GetTimestamp();
                    if (nowTicks >= deadlineTicks)
                        break;
                    // HOT PATH: C stamps its reusable payload and then copies
                    // every byte into each native message. Keep the binding
                    // measurement equivalent so untouched large allocations
                    // do not move page-fault and cache-fill work to the native
                    // I/O thread.
                    StampMetricHeader(payload.AsSpan(), RunId, ActivePhase, msgSize,
                        seq, EpochNsFromTimestamp(nowTicks));
                    seq++;
                    try
                    {
                        if (PerfSocketIo.Send(sender, payload, SendFlags.None) <= 0)
                            continue;
                    }
                    catch (ZlinkException ex)
                        when (PerfShared.IsTransientBackpressure(ex.NativeErrno))
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
                Console.Error.WriteLine($"[single-dealer-router] send failed: {ex.Message}");
                sendError = ex;
            }
            finally
            {
                SendStopTokenBlocking(sender, "[single-dealer-router]");
            }
        });
        senderThread.IsBackground = true;
        senderThread.Start();

        try
        {
            while (!stopReceived)
            {
                if (!WaitForInputSignalDriven(poller, events))
                    continue;

                while (TryReceive(receiver, maybe))
                {
                    if (TryGetPayloadPart(maybe, out Message payloadPart))
                    {
                        if (ProcessBody(payloadPart.AsReadOnlySpan()))
                        {
                            stopReceived = true;
                            break;
                        }
                    }
                }
            }
        }
        catch (Exception ex)
        {
            sendError ??= ex;
        }

        senderThread.Join();

        latencySamples = samples;
        receivedOut = received;
        if (sendError != null)
        {
            DebugLog("single_dealer_router_error:active_failed");
            return false;
        }

        return received > 0 && latencySamples.Count > 0;
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

    private static bool IsInterrupted(int errno)
    {
        return PerfShared.IsInterrupted(errno);
    }

    private static bool IsWouldBlock(int errno)
    {
        return PerfShared.IsWouldBlock(errno);
    }
}
