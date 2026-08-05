using System;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;

internal static partial class PerfRunner
{
    private static readonly bool SinglePerfDebugEnabled =
        !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PERF_DEBUG"));

    internal static void DebugLog(string message)
    {
        if (SinglePerfDebugEnabled)
            Console.Error.WriteLine(message);
    }

    internal static bool WaitForConnectionReady(MonitorSocket monitor,
        int timeoutMs)
    {
        using var readyPoller = new MonitorReadyPoller();
        var monitors = new System.Collections.Generic.List<MonitorSocket>
        {
            monitor,
        };
        var activeIndices = new[] { 0 };
        long deadlineTicks = DeadlineTicksFromMilliseconds(timeoutMs);
        while (true)
        {
            long nowTicks = Stopwatch.GetTimestamp();
            if (nowTicks >= deadlineTicks)
                return false;

            try
            {
                while (true)
                {
                    MonitorEvent? evt = monitor.Recv(RecvFlags.DontWait);
                    if (evt == null)
                        break;
                    if (evt.Event == MonitorEventType.ConnectionReady)
                        return true;
                }
            }
            catch (ZlinkException ex) when (IsInterrupted(ex.NativeErrno)
                                            || IsWouldBlock(ex.NativeErrno))
            {
            }

            int ready = readyPoller.Poll(monitors, activeIndices, 1,
                deadlineTicks, nowTicks);
            if (ready < 0)
                return false;
        }
    }

    internal static bool IsExpectedSingleHeader(PerfMetricHeader header,
        int msgSize, uint phase, uint runId = 1)
    {
        return header.RunId == runId
            && header.Phase == phase
            && header.MsgSize == (uint)msgSize;
    }

    internal static bool TryDecodeExpectedSingleHeader(ReadOnlySpan<byte> payload,
        int msgSize, uint phase, out PerfMetricHeader header, uint runId = 1)
    {
        header = default;
        if (!TryDecodeMetricHeader(payload, out header))
            return false;
        return IsExpectedSingleHeader(header, msgSize, phase, runId);
    }

    internal static void PrintResult(string pattern, string transport, int size,
        double thr, double latNs)
    {
        PrintResult(pattern, transport, size, thr, latNs, latNs, latNs);
    }

    internal static void PrintResult(string pattern, string transport, int size,
        double thr, double latNs, double latP95Ns, double latP99Ns,
        double bandwidthMultiplier = 1.0)
    {
        PerfShared.PrintResult(pattern, transport, size, thr, latNs, latP95Ns,
            latP99Ns, bandwidthMultiplier, fixedFormat: false);
    }

    internal static int ResolveSingleConnectReadyTimeoutMs()
    {
        return PerfEnv.ReadPositive("PERF_CONNECT_READY_TIMEOUT_MS", 1000);
    }

    internal static int ResolveSingleDurationSeconds()
    {
        return PerfEnv.ReadPositive("PERF_SINGLE_DURATION_SECONDS", 5);
    }

    internal static int ResolveSingleRcvTimeoutMs()
    {
        return PerfEnv.ReadNonNegative("PERF_SINGLE_RCVTIMEO_MS", 200);
    }

    internal static int ResolveSingleLatencyCount(string pattern)
    {
        _ = pattern;
        return PerfEnv.ReadPositive("PERF_SINGLE_LATENCY_SAMPLE_CAP", 4_000_000);
    }

    internal static int ResolveSinglePubSubReadySettleMs()
    {
        return PerfEnv.ReadNonNegative("PERF_SINGLE_PUBSUB_READY_SETTLE_MS",
            1000);
    }

    internal static int SendBlocking(IMessageSocket socket,
        ReadOnlySpan<byte> buffer,
        SendFlags flags = SendFlags.None)
    {
        return PerfSocketIo.Send(socket, buffer, flags);
    }

    internal static int SendBlocking(IMessageSocket socket, byte[] buffer,
        SendFlags flags = SendFlags.None)
    {
        if (buffer == null)
            throw new ArgumentNullException(nameof(buffer));
        return PerfSocketIo.Send(socket, buffer, flags);
    }

    internal static bool TrySendActiveMessage(IMessageSocket socket,
        ReadOnlySpan<byte> buffer, string tag)
    {
        try
        {
            return PerfSocketIo.Send(socket, buffer, SendFlags.DontWait) > 0;
        }
        catch (ZlinkException ex)
            when (PerfShared.IsTransientBackpressure(ex.NativeErrno))
        {
            return false;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"{tag} send failed: {ex.Message}");
            throw;
        }
    }

    internal static bool TrySendRoutedActiveMessage(IRoutedMessageSocket socket,
        RoutingId routingId, ReadOnlySpan<byte> buffer, string tag)
    {
        try
        {
            return PerfSocketIo.Send(socket, routingId, buffer,
                SendFlags.DontWait) > 0;
        }
        catch (ZlinkException ex)
            when (PerfShared.IsTransientBackpressure(ex.NativeErrno)
                  || PerfShared.IsTransientNetworkError(ex.NativeErrno))
        {
            return false;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"{tag} send failed: {ex.Message}");
            throw;
        }
    }

    internal static bool PublishActiveMessageBlocking(IPublisherSocket socket,
        string topic, ReadOnlySpan<byte> buffer, string tag)
    {
        try
        {
            // HOT PATH: C PUBSUB uses blocking publish so socket HWM applies
            // backpressure. DontWait would repeatedly allocate and discard
            // payloads while full, changing both the workload and throughput.
            return PerfSocketIo.Publish(socket, topic, buffer) > 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"{tag} send failed: {ex.Message}");
            throw;
        }
    }

    internal static bool WaitForInput(IPoller poller, Span<PollEvent> events,
        int timeoutMs)
    {
        try
        {
            int written = poller.Wait(events, TimeSpan.FromMilliseconds(timeoutMs));
            return written > 0;
        }
        catch (ZlinkException ex) when (IsInterrupted(ex.NativeErrno)
                                        || IsWouldBlock(ex.NativeErrno))
        {
            return false;
        }
    }

    // PERF_SINGLE_TEST_POLICY § 1.4: signal-driven (-1) poller wait. A
    // negative TimeSpan maps to an infinite (-1) native timeout, so the
    // receiver blocks until woken by the wire-level stop token (matching C
    // perf_router_router.cpp's blocking recv that ends purely on the stop
    // token). No timer cap, no atomic shutdown flag.
    internal static bool WaitForInputSignalDriven(IPoller poller,
        Span<PollEvent> events)
    {
        try
        {
            int written = poller.Wait(events, Timeout.InfiniteTimeSpan);
            return written > 0;
        }
        catch (ZlinkException ex) when (IsInterrupted(ex.NativeErrno)
                                        || IsWouldBlock(ex.NativeErrno))
        {
            return false;
        }
    }

    // PERF_SINGLE_TEST_POLICY § 1.4: send the wire-level stop token with a
    // bounded retry through transient backpressure so the receiver exits when
    // it observes the token.
    internal static bool SendStopTokenBlocking(IMessageSocket sender, string tag)
    {
        for (int retry = 0; retry < 100; retry++)
        {
            try
            {
                sender.Options.SendTimeout = null;
                if (PerfSocketIo.Send(sender, StopToken.Bytes,
                        SendFlags.None) > 0)
                    return true;
            }
            catch (ZlinkException ex)
                when (PerfShared.IsTransientBackpressure(ex.NativeErrno)
                      || PerfShared.IsTransientNetworkError(ex.NativeErrno))
            {
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"{tag} stop-token send failed: {ex.Message}");
                return false;
            }

            Thread.Sleep(1);
        }

        Console.Error.WriteLine($"{tag} stop-token send failed");
        return false;
    }

    internal static bool SendRoutedStopTokenBlocking(IRoutedMessageSocket sender,
        RoutingId routingId, string tag)
    {
        for (int retry = 0; retry < 5000; retry++)
        {
            try
            {
                sender.Options.SendTimeout = null;
                if (PerfSocketIo.Send(sender, routingId, StopToken.Bytes,
                        SendFlags.None) > 0)
                    return true;
            }
            catch (ZlinkException ex)
                when (PerfShared.IsTransientBackpressure(ex.NativeErrno))
            {
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"{tag} stop-token send failed: {ex.Message}");
                return false;
            }

            Thread.Sleep(1);
        }

        Console.Error.WriteLine($"{tag} stop-token send failed");
        return false;
    }

    internal static void PublishStopTokenBlocking(IPublisherSocket sender,
        string topic, string tag)
    {
        for (int retry = 0; retry < 100; retry++)
        {
            try
            {
                sender.Options.SendTimeout = null;
                if (PerfSocketIo.Publish(sender, topic, StopToken.Bytes,
                        SendFlags.None) > 0)
                    return;
            }
            catch (ZlinkException ex)
                when (PerfShared.IsTransientBackpressure(ex.NativeErrno))
            {
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"{tag} stop-token publish failed: {ex.Message}");
                return;
            }

            Thread.Sleep(1);
        }

        Console.Error.WriteLine($"{tag} stop-token publish failed");
    }

    private static ulong ResolveSingleHwmValue(string specificName)
    {
        ulong hwm = PerfEnv.ReadUInt64("PERF_SINGLE_HWM", 0);
        ulong specific = PerfEnv.ReadUInt64(specificName, 0);
        return specific > 0 ? specific : hwm;
    }

    private static int ResolveSingleMaxSockets()
    {
        int explicitMaxSockets = PerfEnv.ReadPositive("PERF_MAX_SOCKETS", 0);
        return explicitMaxSockets > 0 ? explicitMaxSockets : 0;
    }

    internal static void ApplySingleContextOptions(IContext ctx)
    {
        int ioThreads = PerfEnv.ReadPositive("PERF_IO_THREADS", 1);
        if (ioThreads > 0)
            ctx.Options.IoThreads = ioThreads;

        ctx.Options.AutoHwmEnabled =
            PerfEnv.ReadNonNegative("PERF_CTX_AUTO_HWM_ENABLE", 1) != 0;
        ctx.Options.AutoHwmProfile = ResolveContextAutoHwmProfile();

        int maxSockets = ResolveSingleMaxSockets();
        if (maxSockets > 0)
            ctx.Options.MaxSockets = maxSockets;
    }

    internal static AutoHwmProfile ResolveContextAutoHwmProfile()
    {
        string value = PerfEnv.ReadString("PERF_CTX_AUTO_HWM_PROFILE", string.Empty);
        if (string.IsNullOrWhiteSpace(value))
            value = PerfEnv.ReadString("PERF_AUTO_HWM_PROFILE", string.Empty);

        return value.Trim().ToLowerInvariant() switch
        {
            "compact" => AutoHwmProfile.Compact,
            "low_latency" or "low-latency" => AutoHwmProfile.LowLatency,
            "throughput" => AutoHwmProfile.Throughput,
            _ => AutoHwmProfile.Balanced,
        };
    }

    internal static void ApplySingleSocketOptions(ISocket socket)
    {
        int sndTimeo = PerfEnv.ReadNonNegative("PERF_SINGLE_SNDTIMEO_MS", 200);
        int rcvTimeo = PerfEnv.ReadNonNegative("PERF_SINGLE_RCVTIMEO_MS", 200);
        bool allowManualOverrides =
            PerfEnv.ReadNonNegative("PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) != 0 ||
            PerfEnv.ReadNonNegative("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) != 0;

        socket.Options.Linger = TimeSpan.Zero;
        socket.Options.SendTimeout = TimeSpan.FromMilliseconds(sndTimeo);
        socket.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeo);
        if (allowManualOverrides)
        {
            ulong sndHwm = ResolveSingleHwmValue("PERF_SINGLE_SNDHWM");
            ulong rcvHwm = ResolveSingleHwmValue("PERF_SINGLE_RCVHWM");
            if (sndHwm > 0)
                socket.Options.SendHighWaterMark = sndHwm;
            if (rcvHwm > 0)
                socket.Options.ReceiveHighWaterMark = rcvHwm;
        }
    }

    internal static void ApplySingleAutoHwmMsgUnit(IContext ctx, int msgSize)
    {
        if (msgSize <= 0)
            return;
        ctx.Options.AutoHwmMessageUnitBytes = (ulong)msgSize;
    }

    internal static void RecalculateSingleAutoHwm(IContext ctx)
    {
        ctx.RecalculateAutoHwm();
    }

}
