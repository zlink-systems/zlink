using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;

internal static partial class PerfRunner
{
    // Wire-level stop token shared with single perf and other bindings.
    // PERF_MULTI_TEST_POLICY § 1.3.1 / PERF_SINGLE_TEST_POLICY § 1.4.
    internal static readonly byte[] MultiStopToken = StopToken.Bytes;

    internal const int MaxStreamFrameBytes = 16 * 1024 * 1024;
    internal const int MaxBurstPerSender = 64;
    internal const ulong MultiSettleNs = 500_000_000UL;

    internal struct MultiSenderRotation(int senderCount)
    {
        private int _accepted;
        internal int Index { get; private set; }

        internal void Advance()
        {
            if (++Index == senderCount)
                Index = 0;
            _accepted = 0;
        }

        internal bool Accepted()
        {
            if (++_accepted < MaxBurstPerSender)
                return false;
            Advance();
            return true;
        }
    }

    internal sealed class MultiPacedReplyTurn(Action submitDue)
    {
        private int _received;

        internal void Received()
        {
            if (++_received == MaxBurstPerSender)
            {
                _received = 0;
                submitDue();
            }
        }
    }

    internal enum PerfPhase : byte
    {
        Warmup = 0,
        Active = 1,
        Cooldown = 2,
        Latency = 3,
    }

    internal static string NormalizePerfPattern(string pattern)
    {
        return PerfShared.NormalizePattern(pattern);
    }

    internal static int ResolveMultiClients(PerfOptions options)
    {
        return options.Clients;
    }

    internal static int ResolveMultiDurationSeconds(PerfOptions options)
    {
        return options.DurationSeconds;
    }

    internal static int ResolveMultiLatencyDurationSeconds(PerfOptions options)
    {
        return PerfEnv.ReadPositive("PERF_MULTI_LATENCY_DURATION_SECONDS",
            options.DurationSeconds);
    }

    internal static double MultiTargetRate(long activeReceived,
        int activeSeconds)
    {
        return activeSeconds > 0
            ? activeReceived / (double)activeSeconds * 0.8
            : 0.0;
    }

    internal static bool MultiLatencySample(ulong sentNs, ulong epochNs,
        int latencySeconds)
    {
        ulong start = epochNs + MultiSettleNs;
        return sentNs >= start
            && sentNs < start + (ulong)latencySeconds * 1_000_000_000UL;
    }

    internal sealed class MultiPacedSchedule
    {
        private const ulong MaxWaitMs = 50;
        private const ulong NsPerMs = 1_000_000UL;

        internal readonly double Rate;
        internal readonly ulong EpochNs;
        private readonly long _epochTicks;
        private long _submittedCount;
        private long _late1To2, _late2To5, _late5To15, _lateOver15;
        private WaitStats _sleepWaits, _pollWaits;

        internal long AcceptedCount { get; private set; }
        internal long LateCount => _late1To2 + _late2To5 + _late5To15 + _lateOver15;

        internal MultiPacedSchedule(double rate)
        {
            Rate = rate;
            _epochTicks = Stopwatch.GetTimestamp();
            EpochNs = PerfShared.EpochNsFromTimestamp(_epochTicks);
        }

        private ulong NextNs => EpochNs + (Rate > 0.0
            ? (ulong)(_submittedCount * 1_000_000_000.0 / Rate) : 0);

        internal bool Due(ulong nowNs) => Rate > 0.0 && nowNs >= NextNs;

        internal ulong EndNs(int latencySeconds)
            => PerfShared.EpochNsFromTimestamp(EndTicks(latencySeconds));

        internal long EndTicks(int latencySeconds)
            => _epochTicks + latencySeconds * Stopwatch.Frequency
                + (long)MultiSettleNs * Stopwatch.Frequency / 1_000_000_000L;

        internal int PollWaitMilliseconds(ulong nowNs, ulong endNs, bool allBlocked)
            => allBlocked && Due(nowNs)
                ? MillisecondsUntil(nowNs, endNs)
                : WaitMilliseconds(nowNs, endNs);

        internal int WaitMilliseconds(ulong nowNs, ulong endNs)
            => MillisecondsUntil(nowNs, Math.Min(endNs, NextNs));

        internal static int MillisecondsUntil(ulong nowNs, ulong deadlineNs)
        {
            if (nowNs >= deadlineNs)
                return 0;

            // Floor to ms and cap at 50 ms. Under 1 ms, recheck now.
            ulong ms = (deadlineNs - nowNs) / NsPerMs;
            return (int)Math.Min(ms, MaxWaitMs);
        }

        // The submit loop owns the sequence. Completion order can differ
        // from submission order, so each admission keeps its own due time.
        internal ulong Submitted()
        {
            ulong dueNs = NextNs;
            _submittedCount++;
            return dueNs;
        }

        internal void Accepted(ulong nowNs, ulong dueNs)
        {
            AcceptedCount++;
            if (nowNs <= dueNs)
                return;
            ulong lateNs = nowNs - dueNs;
            if (lateNs > 15 * NsPerMs)
                _lateOver15++;
            else if (lateNs > 5 * NsPerMs)
                _late5To15++;
            else if (lateNs > 2 * NsPerMs)
                _late2To5++;
            else if (lateNs > NsPerMs)
                _late1To2++;
        }

        internal void Sleep(int requestedMs)
        {
            long start = Stopwatch.GetTimestamp();
            Thread.Sleep(requestedMs);
            _sleepWaits.Record(requestedMs, Stopwatch.GetTimestamp() - start);
        }

        internal void RecordPollWait(int requestedMs, long startTicks)
            => _pollWaits.Record(requestedMs, Stopwatch.GetTimestamp() - startTicks);

        internal void PrintTimingDiag(string pattern, int msgSize)
        {
            WriteStdoutLine(FormattableString.Invariant($"PACED_DIAG,{pattern},{msgSize},late_1_to_2ms,{_late1To2},late_2_to_5ms,{_late2To5},late_5_to_15ms,{_late5To15},late_gt_15ms,{_lateOver15}"));
            _sleepWaits.Print(pattern, msgSize, "sleep");
            _pollWaits.Print(pattern, msgSize, "poll");
        }

        private struct WaitStats
        {
            private long _count, _requestedMs, _elapsedTicks, _maxElapsedTicks;

            internal void Record(int requestedMs, long elapsedTicks)
            {
                _count++;
                _requestedMs += requestedMs;
                _elapsedTicks += elapsedTicks;
                _maxElapsedTicks = Math.Max(_maxElapsedTicks, elapsedTicks);
            }

            internal readonly void Print(string pattern, int msgSize, string kind)
            {
                double ticksToMs = 1000.0 / Stopwatch.Frequency;
                WriteStdoutLine(FormattableString.Invariant($"PACED_DIAG,{pattern},{msgSize},wait_kind,{kind},wait_count,{_count},requested_ms_total,{_requestedMs},elapsed_ms_total,{_elapsedTicks * ticksToMs:F3},elapsed_ms_max,{_maxElapsedTicks * ticksToMs:F3}"));
            }
        }
    }

    internal static void PrintMultiBenchDiag(string pattern, int msgSize,
        long activeSends, int activeSeconds, MultiPacedSchedule schedule,
        long activeReceived, double pacedReceivedRate, bool caughtUp = false,
        double catchUpSeconds = -1.0)
    {
        WriteStdoutLine(FormattableString.Invariant($"BENCH_DIAG,{pattern},{msgSize},active_sender_rate,{activeSends / (double)activeSeconds:G12},target_sender_rate,{schedule.Rate:G12},latency_sends,{schedule.AcceptedCount},late_sends_gt_1ms,{schedule.LateCount},received_rate,{activeReceived / (double)activeSeconds:G12},paced_received_rate,{pacedReceivedRate:G12},caught_up,{(caughtUp ? "yes" : "no")},catch_up_seconds,{catchUpSeconds:G12}"));
        schedule.PrintTimingDiag(pattern, msgSize);
    }

    internal static int ResolveMultiSndTimeoutMs(PerfOptions options)
    {
        return options.SndTimeoutMs;
    }

    internal static int ResolveMultiRcvTimeoutMs(PerfOptions options)
    {
        return options.RcvTimeoutMs;
    }

    internal static int ResolveMultiConnectReadyTimeoutMs(PerfOptions options)
    {
        return options.ConnectReadyTimeoutMs;
    }

    internal static int ResolveMultiSendDrainTimeoutMs()
    {
        return PerfEnv.ReadPositive("PERF_MULTI_SEND_DRAIN_TIMEOUT_MS", 5000);
    }

    internal static ulong ResolveMultiMonitorHwmBytes()
    {
        return PerfEnv.ReadUInt64("PERF_MULTI_MONITOR_HWM_BYTES",
            PerfEnv.ReadUInt64("PERF_MONITOR_HWM_BYTES", 4_096_000UL));
    }

    // PERF_MULTI_TEST_POLICY § 1.3.1: signal-driven receive/relay loops use
    // -1. Echo requester loops own the active clock and bound each wait by
    // their remaining interval at the call site.
    internal const int MultiClientPollTimeoutMs = -1;

    internal static int ResolveMultiClientPollTimeoutMs(PerfOptions options)
    {
        _ = options;
        return MultiClientPollTimeoutMs;
    }

    internal static string MultiEndpointFor(string transport, string name,
        PerfOptions options)
    {
        int bindPort = options.ServerBindPort;
        if (bindPort > 0)
            return $"{transport}://127.0.0.1:{bindPort}";
        return EndpointFor(transport, name);
    }

    internal static bool IsMonitorReady(MonitorEventType eventValue)
    {
        return eventValue == (MonitorEventType)SocketEvent.ConnectionReady;
    }

    internal static bool WaitConnectReadyCount(MonitorSocket monitor,
        int expectedReady, int timeoutMs, Func<int>? startState = null)
    {
        if (expectedReady <= 0)
            return true;

        int readyCount = DrainReadyEvents(monitor);
        if (readyCount >= expectedReady)
            return true;

        using var readyPoller = new MonitorReadyPoller();
        long deadlineTicks = startState == null
            ? DeadlineTicksFromMilliseconds(timeoutMs) : 0;
        while (true)
        {
            long nowTicks = Stopwatch.GetTimestamp();
            if (deadlineTicks == 0)
            {
                int state = startState!();
                if (state < 0)
                    return false;
                if (state > 0)
                    deadlineTicks = DeadlineTicksFromMilliseconds(timeoutMs);
            }
            if (deadlineTicks != 0 && nowTicks >= deadlineTicks)
                return false;

            int rc = readyPoller.Poll(
                new System.Collections.Generic.List<MonitorSocket> { monitor },
                new[] { 0 }, 1,
                deadlineTicks != 0 ? deadlineTicks
                    : DeadlineTicksFromMilliseconds(50), nowTicks);
            if (rc < 0)
                return false;
            if (rc == 0)
                continue;

            try
            {
                readyCount = DrainReadyEvents(monitor, readyCount);
                if (readyCount >= expectedReady)
                    return true;
            }
            catch (ZlinkException ex) when (IsWouldBlock(ex.NativeErrno)
                                            || IsInterrupted(ex.NativeErrno))
            {
            }
            catch
            {
                return false;
            }
        }
    }

    internal static bool IsStopTokenPayload(ReadOnlySpan<byte> payload)
    {
        return StopToken.IsStopToken(payload);
    }

    internal static bool ManualSocketOverridesEnabled()
    {
        return PerfEnv.ReadPositive("PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) > 0
            || PerfEnv.ReadPositive("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) > 0;
    }

    internal static AutoHwmProfile ResolveContextAutoHwmProfile()
    {
        string value = PerfEnv.ReadString("PERF_CTX_AUTO_HWM_PROFILE", string.Empty);
        if (string.IsNullOrWhiteSpace(value))
            value = PerfEnv.ReadString("PERF_AUTO_HWM_PROFILE", string.Empty);

        return value switch
        {
            "compact" => AutoHwmProfile.Compact,
            "low_latency" or "low-latency" => AutoHwmProfile.LowLatency,
            "throughput" => AutoHwmProfile.Throughput,
            _ => AutoHwmProfile.Balanced,
        };
    }

    internal static void ApplyMultiServerContextOptions(IContext ctx,
        PerfOptions options)
    {
        if (options.IoThreads > 0)
            ctx.Options.IoThreads = options.IoThreads;

        if (options.MaxSockets > 0)
            ctx.Options.MaxSockets = options.MaxSockets;

        ctx.Options.Blocky = PerfEnv.ReadBool("PERF_CTX_BLOCKY", false);
        ctx.Options.AutoHwmEnabled =
            PerfEnv.ReadNonNegative("PERF_CTX_AUTO_HWM_ENABLE", 1) != 0;
        ctx.Options.CoreHwmProfile = ResolveContextAutoHwmProfile();
    }

    internal static void ApplyMultiClientContextOptions(IContext ctx,
        PerfOptions options)
    {
        ApplyMultiServerContextOptions(ctx, options);
    }

    internal static void ApplyMultiSocketOptions(ISocket socket,
        PerfOptions options)
    {
        int sndTimeo = ResolveMultiSndTimeoutMs(options);
        int rcvTimeo = ResolveMultiRcvTimeoutMs(options);

        socket.Options.Linger = TimeSpan.Zero;
        if (ManualSocketOverridesEnabled())
        {
            ulong sndHwm = options.ResolveMultiHwm("PERF_MULTI_SNDHWM");
            ulong rcvHwm = options.ResolveMultiHwm("PERF_MULTI_RCVHWM");
            if (sndHwm > 0)
                socket.Options.SendHighWaterMark = sndHwm;
            if (rcvHwm > 0)
                socket.Options.ReceiveHighWaterMark = rcvHwm;
            if (options.MultiSndBuf > 0)
                socket.Options.SendBufferSize = options.MultiSndBuf;
            if (options.MultiRcvBuf > 0)
                socket.Options.ReceiveBufferSize = options.MultiRcvBuf;
        }
        socket.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeo);
    }

    internal static void RecalculateAutoHwm(IContext ctx)
    {
        try
        {
            ctx.RecalculateAutoHwm();
        }
        catch (ZlinkException)
        {
        }
    }

    private static readonly object AutoHwmDetailLock = new();
    private static readonly HashSet<string> AutoHwmDetailSeen =
        new(StringComparer.Ordinal);

    internal static bool AutoHwmDetailEnabled()
    {
        string value = PerfEnv.ReadString("PERF_MULTI_PRINT_AUTO_HWM_DETAIL",
            string.Empty);
        if (string.IsNullOrEmpty(value))
            value = PerfEnv.ReadString("PERF_PRINT_AUTO_HWM_DETAIL",
                string.Empty);
        return string.IsNullOrEmpty(value)
            || !string.Equals(value, "0", StringComparison.Ordinal);
    }

    internal static void PrintAutoHwmSnapshot(ISocket socket, string label,
        string transport, int msgSize)
    {
        if (!AutoHwmDetailEnabled())
            return;

        SocketType socketType = ResolveSocketType(socket);
        MonitorStatus? snapshot = TryReadSocketMonitorStatus(socket);
        bool snapshotFromMonitor = snapshot != null;
        AutoHwmSnapshotFields fields = snapshotFromMonitor
            ? AutoHwmSnapshotFields.FromSnapshot(snapshot!)
            : AutoHwmSnapshotFields.FromSocketOptions(socket);

        PrintAutoHwmDetailLine(label, transport, msgSize,
            SocketTypeName(socketType),
            snapshotFromMonitor ? "monitor_snapshot" : "option_fallback",
            fields);
    }

    private static void PrintAutoHwmDetailLine(string label, string transport,
        int msgSize, string socketType, string source, AutoHwmSnapshotFields fields)
    {
        string pattern = AutoHwmEnvOrDefault("PERF_MULTI_PATTERN", "unknown");
        string component = AutoHwmEnvOrDefault("PERF_MULTI_COMPONENT",
            "process");
        string resolvedTransport = string.IsNullOrEmpty(transport)
            ? AutoHwmEnvOrDefault("PERF_MULTI_TRANSPORT", "unknown")
            : transport;
        string resolvedLabel = string.IsNullOrEmpty(label) ? "socket" : label;

        string detail =
            "AUTO_HWM_DETAIL"
            + $",pattern={pattern}"
            + $",transport={resolvedTransport}"
            + $",component={component}"
            + $",label={resolvedLabel}"
            + $",socket_type={socketType}"
            + $",msg_size={msgSize}"
            + $",source={source}"
            + $",enabled={BoolInt(fields.Enabled)}"
            + $",role={AutoHwmRoleName(fields.Role)}"
            + $",role_id={fields.Role}"
            + $",profile={AutoHwmProfileName(fields.Profile)}"
            + $",profile_id={fields.Profile}"
            + $",policy_class={AutoHwmPolicyClassName(fields.PolicyClass)}"
            + $",policy_class_id={fields.PolicyClass}"
            + $",sndhwm={fields.SndHwm}"
            + $",rcvhwm={fields.RcvHwm}"
            + $",effective_sndbuf={AutoHwmSndbufDisplay(socketType, fields.Role, fields.EffectiveSndbuf)}"
            + $",effective_rcvbuf={AutoHwmRcvbufDisplay(socketType, fields.Role, fields.EffectiveRcvbuf)}"
            + $",last_recalc_ms={fields.LastRecalcMs}"
            + $",last_recalc_reason={AutoHwmRecalcReasonName(fields.LastRecalcReason)}"
            + $",send_blocked_ratio_ppm={fields.SendBlockedRatioPpm}"
            + $",deferred_sndhwm={fields.DeferredSndHwm}"
            + $",deferred_rcvhwm={fields.DeferredRcvHwm}";

        WriteAutoHwmDetailLine(detail);
    }

    private static void WriteAutoHwmDetailLine(string detail)
    {
        lock (AutoHwmDetailLock)
        {
            if (!AutoHwmDetailSeen.Add(detail))
                return;
        }
        WriteStdoutLine(detail);
    }

    private static MonitorStatus? TryReadSocketMonitorStatus(ISocket socket)
    {
        try
        {
            using ISocketMonitor monitor = socket.MonitorOpen(SocketEvent.All);
            return monitor.Status();
        }
        catch (Exception ex) when (ex is ZlinkException
                                  || ex is ObjectDisposedException
                                  || ex is InvalidOperationException)
        {
            return null;
        }
    }

    private static int ReadSocketOption(Func<int> getter)
    {
        try
        {
            return getter();
        }
        catch (ZlinkException)
        {
            return 0;
        }
    }

    private static ulong ReadSocketOption(Func<ulong> getter)
    {
        try
        {
            return getter();
        }
        catch (ZlinkException)
        {
            return 0;
        }
    }

    private static SocketType ResolveSocketType(ISocket socket)
        => socket switch
        {
            IDealerSocket => SocketType.Dealer,
            IRouterSocket => SocketType.Router,
            IPubSocket => SocketType.Pub,
            ISubSocket => SocketType.Sub,
            IStreamSocket => SocketType.Stream,
            _ => SocketType.Any,
        };

    private static string SocketTypeName(SocketType type)
        => type switch
        {
            SocketType.Pair => "pair",
            SocketType.Pub => "pub",
            SocketType.Sub => "sub",
            SocketType.Dealer => "dealer",
            SocketType.Router => "router",
            SocketType.XPub => "xpub",
            SocketType.XSub => "xsub",
            SocketType.Stream => "stream",
            _ => "any",
        };

    private static string AutoHwmEnvOrDefault(string name, string fallback)
    {
        string value = PerfEnv.ReadString(name, string.Empty);
        return string.IsNullOrEmpty(value) ? fallback : value;
    }

    private static int BoolInt(bool value) => value ? 1 : 0;

    // Keep buffer visibility consistent with the raw socket direction.
    private static bool AutoHwmSendSideVisible(string socketType, uint role)
    {
        string roleName = AutoHwmRoleName(role);
        if ((socketType == "sub" || socketType == "xsub")
            && (roleName == "recv_ingress" || roleName == "control"))
            return false;
        return true;
    }

    private static bool AutoHwmRecvSideVisible(string socketType, uint role)
    {
        string roleName = AutoHwmRoleName(role);
        if ((socketType == "pub" || socketType == "xpub")
            && roleName == "control")
            return false;
        return true;
    }

    private static string AutoHwmSndbufDisplay(string socketType, uint role,
        int effectiveSndbuf)
        => AutoHwmSendSideVisible(socketType, role)
            ? $"{effectiveSndbuf}"
            : "0";

    private static string AutoHwmRcvbufDisplay(string socketType, uint role,
        int effectiveRcvbuf)
        => AutoHwmRecvSideVisible(socketType, role)
            ? $"{effectiveRcvbuf}"
            : "0";

    private static string AutoHwmRoleName(uint role)
        => role switch
        {
            1 => "control",
            2 => "routed",
            3 => "fanout",
            4 => "recv_ingress",
            6 => "peer_queue",
            7 => "stream",
            _ => "none",
        };

    private static string AutoHwmProfileName(AutoHwmProfile profile)
        => profile switch
        {
            AutoHwmProfile.Compact => "compact",
            AutoHwmProfile.LowLatency => "low_latency",
            AutoHwmProfile.Balanced => "balanced",
            AutoHwmProfile.Throughput => "throughput",
            _ => "unknown",
        };

    private static string AutoHwmPolicyClassName(uint policyClass)
        => policyClass switch
        {
            1 => "fanout",
            3 => "recv_ingress",
            4 => "routed",
            5 => "peer_queue",
            6 => "stream",
            7 => "control",
            _ => "none",
        };

    private static string AutoHwmRecalcReasonName(AutoHwmRecalcReason reason)
        => reason switch
        {
            AutoHwmRecalcReason.Initial => "initial",
            AutoHwmRecalcReason.RoleChange => "role_change",
            AutoHwmRecalcReason.PolicyToggle => "policy_toggle",
            AutoHwmRecalcReason.Refresh => "refresh",
            AutoHwmRecalcReason.DeferredShrink => "deferred_shrink",
            _ => "none",
        };

    private readonly struct AutoHwmSnapshotFields
    {
        private AutoHwmSnapshotFields(bool enabled, AutoHwmProfile profile, uint role,
            uint policyClass, ulong sndHwm, ulong rcvHwm, int effectiveSndbuf,
            int effectiveRcvbuf, ulong lastRecalcMs,
            AutoHwmRecalcReason lastRecalcReason,
            uint sendBlockedRatioPpm, ulong deferredSndHwm,
            ulong deferredRcvHwm)
        {
            Enabled = enabled;
            Profile = profile;
            Role = role;
            PolicyClass = policyClass;
            SndHwm = sndHwm;
            RcvHwm = rcvHwm;
            EffectiveSndbuf = effectiveSndbuf;
            EffectiveRcvbuf = effectiveRcvbuf;
            LastRecalcMs = lastRecalcMs;
            LastRecalcReason = lastRecalcReason;
            SendBlockedRatioPpm = sendBlockedRatioPpm;
            DeferredSndHwm = deferredSndHwm;
            DeferredRcvHwm = deferredRcvHwm;
        }

        internal bool Enabled { get; }
        internal AutoHwmProfile Profile { get; }
        internal uint Role { get; }
        internal uint PolicyClass { get; }
        internal ulong SndHwm { get; }
        internal ulong RcvHwm { get; }
        internal int EffectiveSndbuf { get; }
        internal int EffectiveRcvbuf { get; }
        internal ulong LastRecalcMs { get; }
        internal AutoHwmRecalcReason LastRecalcReason { get; }
        internal uint SendBlockedRatioPpm { get; }
        internal ulong DeferredSndHwm { get; }
        internal ulong DeferredRcvHwm { get; }

        internal static AutoHwmSnapshotFields FromSnapshot(
            MonitorStatus snapshot)
            => new(snapshot.AutoHwmEnabled, snapshot.AutoHwmProfile,
                snapshot.AutoHwmRole, snapshot.AutoHwmPolicyClass,
                snapshot.AutoHwmAppliedSendHighWaterMarkBytes,
                snapshot.AutoHwmAppliedReceiveHighWaterMarkBytes,
                snapshot.AutoHwmEffectiveSndbuf,
                snapshot.AutoHwmEffectiveRcvbuf,
                snapshot.AutoHwmLastRecalcMs,
                snapshot.AutoHwmLastRecalcReason,
                snapshot.AutoHwmSendBlockedRatioPpm,
                snapshot.AutoHwmDeferredSendHighWaterMarkBytes,
                snapshot.AutoHwmDeferredReceiveHighWaterMarkBytes);

        internal static AutoHwmSnapshotFields FromSocketOptions(ISocket socket)
            => new(false, (AutoHwmProfile)0, 0, 0,
                ReadSocketOption(() => socket.Options.SendHighWaterMark),
                ReadSocketOption(() => socket.Options.ReceiveHighWaterMark),
                ReadSocketOption(() => socket.Options.SendBufferSize),
                ReadSocketOption(() => socket.Options.ReceiveBufferSize),
                0, (AutoHwmRecalcReason)0, 0, 0, 0);
    }

    internal static int ResolveMultiLatencySampleCap(PerfOptions options)
    {
        return options.LatencySampleCap;
    }

    internal static void ReservoirSampleMulti(List<double> samples,
        double value, ref long seenCount, ref double exactSum, int cap,
        ref uint rngState)
    {
        seenCount++;
        exactSum += value;
        if (cap <= 0)
            return;
        if (samples.Count < cap)
        {
            samples.Add(value);
            return;
        }

        rngState = unchecked(rngState * 1664525u + 1013904223u);
        ulong slot = rngState % (ulong)seenCount;
        if (slot < (ulong)samples.Count)
            samples[(int)slot] = value;
    }

    internal static (double mean, double p95, double p99)
        ComputeMultiLatencyStats(List<double> samples, long exactCount,
            double exactSum)
    {
        if (exactCount <= 0)
            return (0.0, 0.0, 0.0);

        double mean = exactSum / exactCount;
        if (samples.Count == 0)
            return (mean, mean, mean);

        samples.Sort();
        return (mean, Percentile(samples, 0.95), Percentile(samples, 0.99));

        static double Percentile(IReadOnlyList<double> sorted, double q)
        {
            double pos = (sorted.Count - 1) * q;
            int lo = (int)pos;
            int hi = lo + 1 < sorted.Count ? lo + 1 : lo;
            double fraction = pos - lo;
            return sorted[lo] + (sorted[hi] - sorted[lo]) * fraction;
        }
    }

    internal static bool IsCoreStreamServerTransport(string transport)
    {
        return transport.Equals("tcp", StringComparison.OrdinalIgnoreCase)
            || transport.Equals("tls", StringComparison.OrdinalIgnoreCase)
            || transport.Equals("ws", StringComparison.OrdinalIgnoreCase)
            || transport.Equals("wss", StringComparison.OrdinalIgnoreCase);
    }

}
