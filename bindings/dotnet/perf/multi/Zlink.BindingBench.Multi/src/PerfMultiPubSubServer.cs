using System;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiPubSubServer
{
    private const string Topic = "bench";

    internal static int Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        int durationSeconds = ResolveMultiDurationSeconds(options);
        int latencyDurationSeconds = ResolveMultiLatencyDurationSeconds(options);
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
        long activeSends = 0;

        if (!RunPublishPhase(server, payloadSize,
                runId, size, ref seq, PerfPhase.Active, durationSeconds,
                controlState, ref activeSends)
            || !PublishStopToken(server, size, runId,
                PerfPhase.Active, controlState))
        {
            return 2;
        }

        WriteStdoutLine($"PHASE_DONE,{size},1");
        if (!controlState.WaitForPhase("PHASE_PACED", readyTimeoutMs,
                out long activeReceived) || activeReceived <= 0)
            return 2;

        var schedule = new MultiPacedSchedule(
            MultiTargetRate(activeReceived, durationSeconds));
        ulong catchupDeadlineNs = schedule.EpochNs
            + (ulong)latencyDurationSeconds * 1_000_000_000UL;
        ulong phaseDeadlineNs = catchupDeadlineNs
            + (ulong)latencyDurationSeconds * 1_000_000_000UL;
        ulong caughtUpNs = 0;
        while (!controlState.StopRequested && EpochNs() < phaseDeadlineNs)
        {
            ulong nowNs = EpochNs();
            if (caughtUpNs == 0
                && controlState.TryGetPhase("PHASE_CATCHUP",
                    out ulong receiptNs)
                && receiptNs <= catchupDeadlineNs)
                caughtUpNs = receiptNs;
            ulong endNs = caughtUpNs != 0
                ? caughtUpNs + (ulong)latencyDurationSeconds
                    * 1_000_000_000UL : phaseDeadlineNs;
            if (nowNs >= endNs)
                break;
            if (!schedule.Due(nowNs))
            {
                int waitMs = schedule.WaitMilliseconds(nowNs, endNs);
                if (waitMs > 0)
                    schedule.Sleep(waitMs);
                continue;
            }
            using Message message = Message.Allocate(payloadSize);
            StampMetricHeader(message.AsSpan(), runId,
                caughtUpNs != 0 && nowNs >= caughtUpNs
                    ? PerfPhase.Latency : PerfPhase.Warmup,
                size, seq++, nowNs);
            if (!Publish(server, message))
                return 2;
            schedule.Accepted(EpochNs(), schedule.Submitted());
        }
        if (!PublishStopToken(server, size, runId,
                PerfPhase.Latency, controlState))
            return 2;
        WriteStdoutLine($"PHASE_DONE,{size},3");
        if (!controlState.WaitForPhase("PHASE_COMPLETE",
                Math.Max(readyTimeoutMs,
                    2 * latencyDurationSeconds * 1000 + 10_000),
                out long pacedReceived))
            return 2;
        PrintMultiBenchDiag("PUBSUB", size, activeSends, durationSeconds,
            schedule, activeReceived,
            pacedReceived / (double)latencyDurationSeconds,
            caughtUpNs != 0,
            caughtUpNs != 0
                ? (caughtUpNs - schedule.EpochNs) / 1_000_000_000.0
                : -1.0);
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

    private static bool PublishStopToken(IPubSocket server, int size,
        uint runId, PerfPhase phase,
        RunnerControlState controlState)
    {
        while (!controlState.StopRequested)
        {
            try
            {
                using Message message = Message.Allocate(
                    PerfMetricHeaderSize);
                StampMetricHeader(message.AsSpan(), runId, phase,
                    size, 0, 0);
                if (Publish(server, message))
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
        RunnerControlState controlState, ref long sendCount)
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
            if (Stopwatch.GetTimestamp() < deadlineTicks)
                sendCount++;
        }

        return true;
    }

}
