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

    internal enum PerfPhase : byte
    {
        Warmup = 0,
        Active = 1,
        Cooldown = 2,
    }

    internal static string NormalizePerfPattern(string pattern)
    {
        return PerfShared.NormalizePattern(pattern, trimMultiPrefix: true);
    }

    internal static int ResolveMultiClients(PerfOptions options)
    {
        return options.Clients;
    }

    internal static int ResolveMultiDurationSeconds(PerfOptions options)
    {
        return options.DurationSeconds;
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

    // PERF_MULTI_TEST_POLICY § 1.3.1: client/server poller wait timeouts
    // are unconditionally -1 (signal-driven wait). Deadlines are tracked
    // by application clock; phase end / shutdown is signaled either by
    // stdin STOP (server) or wire-level stop token (client/receiver).
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
        int expectedReady, int timeoutMs)
    {
        if (expectedReady <= 0)
            return true;

        int readyCount = DrainReadyEvents(monitor);
        if (readyCount >= expectedReady)
            return true;

        using var readyPoller = new MonitorReadyPoller();
        long deadlineTicks = DeadlineTicksFromMilliseconds(timeoutMs);
        while (true)
        {
            long nowTicks = Stopwatch.GetTimestamp();
            if (nowTicks >= deadlineTicks)
                return false;

            int rc = readyPoller.Poll(
                new System.Collections.Generic.List<MonitorSocket> { monitor },
                new[] { 0 }, 1, deadlineTicks, nowTicks);
            if (rc < 0)
                return false;
            if (rc == 0)
                continue;

            try
            {
                readyCount += DrainReadyEvents(monitor);
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
        ctx.Options.AutoHwmProfile = ResolveContextAutoHwmProfile();
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
        socket.Options.SendTimeout = TimeSpan.FromMilliseconds(sndTimeo);
        socket.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeo);
    }

    internal static void ApplyAutoHwmMsgUnit(IContext ctx, int msgSize)
    {
        if (msgSize <= 0)
            return;
        try
        {
            ctx.Options.AutoHwmMessageUnitBytes = (ulong)msgSize;
        }
        catch (ZlinkException)
        {
        }
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
            + $",unit_budget_bytes={fields.UnitBudgetBytes}"
            + $",size_cap={fields.SizeCap}"
            + $",sndhwm={fields.SndHwm}"
            + $",rcvhwm={fields.RcvHwm}"
            + $",socket_message_slots={fields.SocketMessageSlots}"
            + $",effective_message_bytes={fields.EffectiveMessageBytes}"
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
            uint policyClass, ulong unitBudgetBytes, uint sizeCap,
            ulong socketMessageSlots, ulong effectiveMessageBytes,
            ulong sndHwm, ulong rcvHwm, int effectiveSndbuf,
            int effectiveRcvbuf, ulong lastRecalcMs,
            AutoHwmRecalcReason lastRecalcReason,
            uint sendBlockedRatioPpm, ulong deferredSndHwm,
            ulong deferredRcvHwm)
        {
            Enabled = enabled;
            Profile = profile;
            Role = role;
            PolicyClass = policyClass;
            UnitBudgetBytes = unitBudgetBytes;
            SizeCap = sizeCap;
            SocketMessageSlots = socketMessageSlots;
            EffectiveMessageBytes = effectiveMessageBytes;
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
        internal ulong UnitBudgetBytes { get; }
        internal uint SizeCap { get; }
        internal ulong SocketMessageSlots { get; }
        internal ulong EffectiveMessageBytes { get; }
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
                snapshot.AutoHwmUnitBudgetBytes, snapshot.AutoHwmSizeCap,
                snapshot.AutoHwmSocketMessageSlots,
                snapshot.AutoHwmEffectiveMessageBytes,
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
            => new(false, (AutoHwmProfile)0, 0, 0, 0, 0, 0, 0,
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

    internal static int ResolveMultiOnewayLatencySampleStride()
    {
        return PerfEnv.ReadPositive("PERF_MULTI_ONEWAY_LATENCY_SAMPLE_STRIDE", 32);
    }

    internal static bool IsCoreStreamServerTransport(string transport)
    {
        return transport.Equals("tcp", StringComparison.OrdinalIgnoreCase)
            || transport.Equals("tls", StringComparison.OrdinalIgnoreCase)
            || transport.Equals("ws", StringComparison.OrdinalIgnoreCase)
            || transport.Equals("wss", StringComparison.OrdinalIgnoreCase);
    }

}
