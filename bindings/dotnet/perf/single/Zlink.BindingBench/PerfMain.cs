if (!PerfOptions.TryParseSingleArgs(args, out PerfOptions options))
    return 1;

if (!SinglePerfPatternRegistry.TryGet(options.Pattern, out IPerfPattern pattern))
    return 1;

return pattern.RunSingle(options);
