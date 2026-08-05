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
        set
        {
            if (!Enum.IsDefined(value))
                throw new ArgumentOutOfRangeException(nameof(value));
            if (_options.LiveLevel is { } cell)
                cell.Level = value;
            else
                _options.SetLevel(value);
            ZLinkTelemetry.SetDiagnosticsLevel(value);
        }
    }
}
