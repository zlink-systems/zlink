using System;
using System.Diagnostics;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiPubSubServer
{
    private const string Topic = "bench";

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
        server.Options.NoDrop = options.PubSubXpubNoDrop > 0;
        server.Options.Linger = TimeSpan.Zero;

        server.Bind(endpoint);
        endpoint = server.Options.LastEndpoint;
        RecalculateAutoHwm(ctx);
        PrintAutoHwmSnapshot(server, "server", options.Transport, size);
        WriteStdoutLine($"READY,{endpoint}");

        if (!controlState.WaitForStart(readyTimeoutMs))
        {
            if (!controlState.StopRequested)
                Console.Error.WriteLine("multi_server_error:no_start");
            return controlState.StopRequested ? 0 : 2;
        }

        const uint runId = 1;
        ulong seq = 1;
        int payloadSize = Math.Max(size, PerfMetricHeaderSize);

        if (!RunPublishPhase(server, payloadSize,
                runId, size, ref seq, PerfPhase.Active, durationSeconds,
                controlState)
            || !PublishStopToken(server, controlState))
        {
            return 2;
        }

        return 0;
    }

    private static bool Publish(IPubSocket server, Message message)
    {
        Message? tail = PerfSocketIo.MeasurementPartCount == 2
            ? Message.Allocate(0) : null;
        try
        {
            var submit = server.TryPublish(Topic).Message(message);
            if (tail != null)
                submit = submit.Message(tail);
            return submit.Flags(SendFlags.None).Submit();
        }
        finally
        {
            tail?.Dispose();
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
                if (server.TryPublish(Topic).Message(message)
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

    private static bool RunPublishPhase(IPubSocket server, int payloadSize,
        uint runId, int size,
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
            if (!Publish(server, message))
                return false;
        }

        return true;
    }

}
