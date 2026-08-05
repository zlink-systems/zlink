using System;

public enum PerfExecutionKind
{
    Single = 0,
    MultiServer = 1,
    MultiClient = 2,
}

public sealed record PerfOptions(
    PerfExecutionKind ExecutionKind,
    string Pattern,
    string Transport,
    int Size,
    string Endpoint,
    string ControlEndpoint,
    int DurationSeconds,
    int SndTimeoutMs,
    int RcvTimeoutMs,
    int ConnectReadyTimeoutMs,
    int Clients,
    int LatencySampleCap,
    int IoThreads,
    int MaxSockets,
    ulong SingleHwm,
    ulong SingleSndHwm,
    ulong SingleRcvHwm,
    ulong MultiHwm,
    ulong MultiSndHwm,
    ulong MultiRcvHwm,
    int MultiSndBuf,
    int MultiRcvBuf,
    int ServerBindPort,
    int PubSubXpubNoDrop)
{
    public static bool TryParseSingleArgs(string[] args, out PerfOptions options)
    {
        options = default!;
        if (args.Length < 3)
            return false;

        string pattern = PerfShared.NormalizePattern(args[0]);
        string transport = args[1];
        if (!int.TryParse(args[2], out int size))
            return false;

        options = CreateSingle(pattern, transport, NormalizeMessageSize(size));
        return true;
    }

    public static bool TryParseMultiArgs(string[] args, out PerfOptions options)
    {
        options = default!;
        if (args.Length >= 4 && string.Equals(args[0], "--multi-server",
                StringComparison.OrdinalIgnoreCase))
        {
            string pattern = PerfShared.NormalizePattern(args[1], true);
            string transport = args[2];
            if (!int.TryParse(args[3], out int size))
                return false;

            options = CreateMulti(PerfExecutionKind.MultiServer, pattern,
                transport, NormalizeMessageSize(size), string.Empty,
                string.Empty);
            return true;
        }

        if (args.Length >= 6 && string.Equals(args[0], "--multi-client",
                StringComparison.OrdinalIgnoreCase))
        {
            string pattern = PerfShared.NormalizePattern(args[1], true);
            string transport = args[2];
            if (!int.TryParse(args[3], out int size))
                return false;

            string endpoint = FindOption(args, "--endpoint", string.Empty);
            endpoint = endpoint?.Trim() ?? string.Empty;
            if (string.IsNullOrWhiteSpace(endpoint))
                return false;

            string controlEndpoint = FindOption(args, "--control-endpoint",
                string.Empty);
            controlEndpoint = controlEndpoint?.Trim() ?? string.Empty;

            options = CreateMulti(PerfExecutionKind.MultiClient, pattern,
                transport, NormalizeMessageSize(size), endpoint,
                controlEndpoint);
            return true;
        }

        return false;
    }

    public static PerfOptions CreateSingle(string pattern, string transport,
        int size)
    {
        return new PerfOptions(
            PerfExecutionKind.Single,
            pattern,
            transport,
            size,
            string.Empty,
            string.Empty,
            PerfEnv.ReadPositive("PERF_SINGLE_DURATION_SECONDS", 5),
            PerfEnv.ReadNonNegative("PERF_SINGLE_SNDTIMEO_MS", 200),
            PerfEnv.ReadNonNegative("PERF_SINGLE_RCVTIMEO_MS", 200),
            PerfEnv.ReadPositive("PERF_CONNECT_READY_TIMEOUT_MS", 1000),
            1,
            PerfEnv.ReadPositive("PERF_SINGLE_LATENCY_SAMPLE_CAP", 200000),
            PerfEnv.ReadNonNegative("PERF_IO_THREADS", 0),
            0,
            PerfEnv.ReadUInt64("PERF_SINGLE_HWM", 0),
            PerfEnv.ReadUInt64("PERF_SINGLE_SNDHWM", 0),
            PerfEnv.ReadUInt64("PERF_SINGLE_RCVHWM", 0),
            0,
            0,
            0,
            0,
            0,
            0,
            PerfEnv.ReadPositive("PERF_SINGLE_PUBSUB_XPUB_NODROP", 1));
    }

    public static PerfOptions CreateMulti(PerfExecutionKind kind, string pattern,
        string transport, int size, string endpoint,
        string controlEndpoint = "")
    {
        int clients = ResolveMultiClients(pattern);
        return new PerfOptions(
            kind,
            pattern,
            transport,
            size,
            endpoint,
            controlEndpoint,
            PerfEnv.ReadPositive("PERF_MULTI_DURATION_SECONDS", 5),
            PerfEnv.ReadPositive("PERF_MULTI_SNDTIMEO_MS", 200),
            PerfEnv.ReadPositive("PERF_MULTI_RCVTIMEO_MS", 200),
            PerfEnv.ReadPositive("PERF_MULTI_CONNECT_READY_TIMEOUT_MS",
                PerfEnv.ReadPositive("PERF_CONNECT_READY_TIMEOUT_MS", 1000)),
            clients,
            PerfEnv.ReadPositive("PERF_MULTI_LATENCY_SAMPLE_CAP", 200000),
            // PERF_POLICY: multi default context I/O threads is 4 for both
            // server and client (C bench_io_threads: PERF_IO_THREADS default 4).
            PerfEnv.ReadPositive(
                "PERF_IO_THREADS",
                PerfEnv.ReadPositive(
                    "PERF_MULTI_DEFAULT_IO_THREADS",
                    PerfEnv.ReadPositive("PERF_DEFAULT_IO_THREADS", 4))),
            ResolveMaxSockets(clients),
            0,
            0,
            0,
            PerfEnv.ReadUInt64("PERF_MULTI_HWM", 0),
            PerfEnv.ReadUInt64("PERF_MULTI_SNDHWM", 0),
            PerfEnv.ReadUInt64("PERF_MULTI_RCVHWM", 0),
            PerfEnv.ReadByteSize("PERF_MULTI_SNDBUF", 0),
            PerfEnv.ReadByteSize("PERF_MULTI_RCVBUF", 0),
            PerfEnv.ReadNonNegative("PERF_MULTI_SERVER_BIND_PORT", 0),
            PerfEnv.ReadPositive("PERF_MULTI_PUBSUB_XPUB_NODROP", 0));
    }

    public ulong ResolveSingleHwm(string specificName)
    {
        ulong specific = PerfEnv.ReadUInt64(specificName, 0);
        return specific > 0 ? specific : SingleHwm;
    }

    public ulong ResolveMultiHwm(string specificName)
    {
        return specificName switch
        {
            "PERF_MULTI_SNDHWM" => MultiSndHwm > 0
                ? MultiSndHwm
                : MultiHwm,
            "PERF_MULTI_RCVHWM" => MultiRcvHwm > 0
                ? MultiRcvHwm
                : MultiHwm,
            _ => MultiHwm,
        };
    }

    private static int ResolveMultiClients(string pattern)
    {
        int fallback = pattern.Equals("STREAM", StringComparison.OrdinalIgnoreCase)
            ? PerfEnv.ReadPositive("PERF_MULTI_DEFAULT_STREAM_CLIENTS", 10000)
            : PerfEnv.ReadPositive("PERF_MULTI_DEFAULT_CLIENTS", 100);
        return Math.Max(1, PerfEnv.ReadPositive("PERF_MULTI_CLIENTS", fallback));
    }

    private static int ResolveMaxSockets(int clients)
    {
        int explicitMaxSockets = PerfEnv.ReadPositive("PERF_MAX_SOCKETS", 0);
        if (explicitMaxSockets > 0)
            return explicitMaxSockets;

        if (clients <= 0)
            return 0;

        long required = (clients * 3L) + 4096L;
        if (required > int.MaxValue)
            return int.MaxValue;
        return (int)required;
    }

    private static string FindOption(string[] args, string name,
        string fallback)
    {
        for (int i = 0; i + 1 < args.Length; i++)
        {
            if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
                return args[i + 1];
        }

        return fallback;
    }

    private static int NormalizeMessageSize(int size)
    {
        return Math.Max(PerfShared.PerfMetricHeaderSize, size);
    }
}
