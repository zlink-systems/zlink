using System;
using System.Diagnostics;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiPubSubServer
{
    private const string Topic = "bench";
    private const int PublishRetryPollTimeoutMs = 100;

    internal static int Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int sndTimeoutMs = ResolveMultiSndTimeoutMs(options);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        int clientCount = ResolveMultiClients(options);
        int durationSeconds = ResolveMultiDurationSeconds(options);
        string endpoint = MultiEndpointFor(options.Transport, "multi-pubsub",
            options);

        using var ctx = Zlink.CreateContext();
        using var controlState = new RunnerControlState(size);
        ApplyMultiServerContextOptions(ctx, options);
        using var server = ctx.CreatePubSocket();
        ApplyMultiSocketOptions(server, options);
        ConfigureTlsServerIfNeeded(server, options.Transport);
        server.Options.SendTimeout = TimeSpan.FromMilliseconds(sndTimeoutMs);
        server.Options.NoDrop = options.PubSubXpubNoDrop > 0;
        server.Options.Linger = TimeSpan.Zero;

        server.Bind(endpoint);
        endpoint = server.Options.LastEndpoint;
        ApplyAutoHwmMsgUnit(ctx, size);
        RecalculateAutoHwm(ctx);
        PrintAutoHwmSnapshot(server, "server", options.Transport, size);
        WriteStdoutLine($"READY,{endpoint}");

        using var sendPoller = Zlink.CreatePoller();
        sendPoller.Add(server, PollEventFlags.PollOut, 0);
        var sendEvents = new PollEvent[1];

        if (!controlState.WaitForStart(readyTimeoutMs))
        {
            if (!controlState.StopRequested)
                Console.Error.WriteLine("multi_server_error:no_start");
            return controlState.StopRequested ? 0 : 2;
        }

        const uint runId = 1;
        ulong seq = 1;
        int payloadSize = Math.Max(size, PerfMetricHeaderSize);

        if (!RunPublishPhase(server, sendPoller, sendEvents, payloadSize,
                runId, size, ref seq, PerfPhase.Active, durationSeconds,
                controlState)
            || !PublishStopToken(server, controlState))
        {
            return 2;
        }

        return 0;
    }

    private static bool PublishNoWait(IPubSocket server, Message message)
    {
        try
        {
            return server.Publish(Topic).Message(message)
                .Flags(SendFlags.DontWait).Submit();
        }
        catch (ZlinkException ex) when (IsWouldBlock(ex.NativeErrno)
                                        || IsInterrupted(ex.NativeErrno))
        {
            return false;
        }
    }

    private static bool PublishStopToken(IPubSocket server,
        RunnerControlState controlState)
    {
        // The active phase uses the same lossy PUB path as C. After the
        // measured window, publish one blocking stop token and finish the
        // server lifecycle. This matches the C runner's single successful
        // stop-token send instead of extending the measured process lifetime.
        while (!controlState.StopRequested)
        {
            try
            {
                using Message message = new(MultiStopToken.AsSpan());
                if (server.Publish(Topic).Message(message)
                        .Flags(SendFlags.None).Submit())
                    return true;
            }
            catch (ZlinkException ex) when (IsTransientStopPublishErrno(
                                                ex.NativeErrno))
            {
            }
        }

        return true;
    }

    private static bool IsTransientStopPublishErrno(int errno)
    {
        return IsWouldBlock(errno) || IsInterrupted(errno) || errno == 110;
    }

    private static bool RunPublishPhase(IPubSocket server, IPoller sendPoller,
        PollEvent[] sendEvents, int payloadSize, uint runId, int size,
        ref ulong seq, PerfPhase phase, int durationSeconds,
        RunnerControlState controlState)
    {
        long deadlineTicks = Stopwatch.GetTimestamp()
            + (long)Math.Max(1, durationSeconds) * Stopwatch.Frequency;
        while (!controlState.StopRequested
               && Stopwatch.GetTimestamp() < deadlineTicks)
        {
            using Message message = Message.Allocate(payloadSize);
            StampMetricHeader(message.AsSpan(), runId, phase, size, seq++,
                EpochNs());
            while (!controlState.StopRequested)
            {
                if (PublishNoWait(server, message))
                    break;
                if (!WaitForWritable(sendPoller, sendEvents, controlState))
                    return false;
            }
        }

        return true;
    }

    private static bool WaitForWritable(IPoller sendPoller,
        PollEvent[] sendEvents, RunnerControlState controlState)
    {
        while (!controlState.StopRequested)
        {
            int ready;
            try
            {
                ready = sendPoller.Wait(sendEvents,
                    TimeSpan.FromMilliseconds(PublishRetryPollTimeoutMs));
            }
            catch (ZlinkException ex) when (IsWouldBlock(ex.NativeErrno)
                                            || IsInterrupted(ex.NativeErrno))
            {
                continue;
            }

            if (ready <= 0)
                continue;
            if ((sendEvents[0].Revents & PollEventFlags.PollOut) != 0)
                return true;
        }

        return false;
    }

}
