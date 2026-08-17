namespace Zlink.Framework.Runtime.Dispatch;

internal sealed class ZLinkDispatchOptionsModel : IZLinkDispatchOptions
{
    public ZLinkUnhandledDispatchOptionsModel Unhandled { get; } = new();

    public ZLinkDiagnosticsOptionsModel Diagnostics { get; } = new();

    IZLinkUnhandledDispatchOptions IZLinkDispatchOptions.Unhandled => Unhandled;

    IZLinkDiagnosticsOptions IZLinkDispatchOptions.Diagnostics => Diagnostics;
}

internal sealed class ZLinkDiagnosticsLevelCell
{
    private int _level;

    public ZLinkDiagnosticsLevelCell(ZLinkDiagnosticsLevel seed)
    {
        _level = (int)seed;
    }

    public ZLinkDiagnosticsLevel Level
    {
        get => (ZLinkDiagnosticsLevel)Volatile.Read(ref _level);
        set => Volatile.Write(ref _level, (int)value);
    }
}

internal sealed class ZLinkUnhandledDispatchOptionsModel : IZLinkUnhandledDispatchOptions
{
    public ZLinkUnhandledDispatchAction Request { get; set; } = ZLinkUnhandledDispatchAction.ReplyError;

    public ZLinkUnhandledDispatchAction Send { get; set; } = ZLinkUnhandledDispatchAction.LogAndDrop;

    public ZLinkUnhandledDispatchAction Publish { get; set; } = ZLinkUnhandledDispatchAction.LogAndDrop;
}

internal sealed class ZLinkDiagnosticsOptionsModel : IZLinkDiagnosticsOptions
{
    private static long _nextSourceGeneration;

    public ulong SourceMeshGeneration { get; } =
        unchecked((ulong)Interlocked.Increment(ref _nextSourceGeneration));

    public ZLinkDiagnosticsLevelCell? LiveLevel { get; internal set; }

    //  Spec 26 §4.1: a level change is an atomic state change every processing
    //  point observes. Before the runtime installs the live cell, EffectiveLevel
    //  reads this seed, so it needs the same volatile visibility as the cell.
    private int _configuredLevel = (int)ZLinkDiagnosticsLevel.Errors;

    public ZLinkDiagnosticsLevel ConfiguredLevel
    {
        get => (ZLinkDiagnosticsLevel)Volatile.Read(ref _configuredLevel);
        private set => Volatile.Write(ref _configuredLevel, (int)value);
    }

    public double SampleRate { get; private set; } = 1.0d;

    public bool MessageSizesIncluded { get; private set; }

    public ZLinkDiagnosticsLevel EffectiveLevel =>
        LiveLevel is { } cell ? cell.Level : ConfiguredLevel;

    public IZLinkDiagnosticsOptions SetLevel(ZLinkDiagnosticsLevel level)
    {
        if (!Enum.IsDefined(level))
            throw new ArgumentOutOfRangeException(nameof(level));
        ConfiguredLevel = level;
        if (LiveLevel is { } cell) cell.Level = level;
        ZLinkTelemetry.SetDiagnosticsLevel(level);
        return this;
    }

    public IZLinkDiagnosticsOptions SetSampleRate(double rate)
    {
        if (double.IsNaN(rate) || rate is < 0.0d or > 1.0d)
            throw new ArgumentOutOfRangeException(nameof(rate));
        SampleRate = rate;
        return this;
    }

    public IZLinkDiagnosticsOptions IncludeMessageSizes(bool include)
    {
        MessageSizesIncluded = include;
        return this;
    }
}
