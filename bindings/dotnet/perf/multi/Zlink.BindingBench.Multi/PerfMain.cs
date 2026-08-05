if (!PerfOptions.TryParseMultiArgs(args, out PerfOptions options))
    Environment.Exit(1);

if (!PerfRunner.IsSupportedTransport(options.Transport))
    Environment.Exit(1);

int rc = options.ExecutionKind == PerfExecutionKind.MultiServer
    ? PerfRunner.RunMultiServer(options.Pattern, options.Transport, options.Size)
    : PerfRunner.RunMultiClient(options.Pattern, options.Transport, options.Size,
        options.Endpoint, options.ControlEndpoint);
Environment.Exit(rc);
