namespace Zlink.Framework.Runtime.Diagnostics;

internal sealed class ZLinkDiagnosticsRuntimeService : IZLinkDiagnosticsRuntime
{
    private readonly ZLinkDiagnosticsOptionsModel _options;

    public ZLinkDiagnosticsRuntimeService(ZLinkDiagnosticsOptionsModel options)
    {
        _options = options;
        ZLinkTelemetry.SetDiagnosticsLevel(options.EffectiveLevel);
    }

    public ZLinkDiagnosticsLevel Level
    {
        get => _options.EffectiveLevel;
        set => SetLevelAsync(value).GetAwaiter().GetResult();
    }

    public Task SetLevelAsync(ZLinkDiagnosticsLevel level)
    {
        if (!Enum.IsDefined(level))
            throw new ArgumentOutOfRangeException(nameof(level));
        if (_options.LiveLevel is { } cell)
            cell.Level = level;
        else
            _options.SetLevel(level);
        ZLinkTelemetry.SetDiagnosticsLevel(level);
        return Task.CompletedTask;
    }
}
