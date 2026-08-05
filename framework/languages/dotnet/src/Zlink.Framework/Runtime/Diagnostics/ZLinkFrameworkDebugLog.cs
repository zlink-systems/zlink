using System.Runtime.CompilerServices;

namespace Zlink.Framework.Runtime.Diagnostics;

internal static class ZLinkFrameworkDebugLog
{
    //  The environment is read once. A debug switch cannot change while the
    //  process runs, and re-reading it turned every call into a lookup that
    //  allocated a string just to be compared and thrown away.
    internal static readonly bool SpotDiscoveryEnabled =
        IsEnabled("ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY");
    private static readonly bool StartupEnabled =
        IsEnabled("ZLINK_DEBUG_FRAMEWORK_STARTUP");
    private static readonly bool TasksEnabled =
        IsEnabled("ZLINK_DEBUG_FRAMEWORK_TASKS");

    public static void Startup(Exception exception)
    {
        if (StartupEnabled) Write(exception.ToString());
    }

    /// <summary>
    /// Records a spot-discovery trace. The message is an interpolated string
    /// handler, so when the switch is off the interpolation never runs and the
    /// call costs one bool test.
    /// </summary>
    public static void SpotDiscovery(ZLinkSpotDiscoveryLogMessage message)
    {
        if (SpotDiscoveryEnabled)
            Write("[zlink-framework-spot-discovery] " + message.ToStringAndClear());
    }

    //  Callers that already hold a built string use this overload directly.
    public static void SpotDiscovery(string message)
    {
        if (SpotDiscoveryEnabled)
            Write("[zlink-framework-spot-discovery] " + message);
    }

    public static void TaskFailure(string taskName, Exception exception)
    {
        if (TasksEnabled) Write($"[zlink-framework] task '{taskName}' failed: {exception}");
    }

    public static void UnhandledCallbackFailure(Exception exception)
    {
        if (TasksEnabled)
            Write($"[zlink-framework] unhandled callback failed: {exception}");
    }

    private static bool IsEnabled(string variableName)
    {
        return Environment.GetEnvironmentVariable(variableName) == "1";
    }

    private static void Write(string message)
    {
        Console.Error.WriteLine(message);
    }
}

/// <summary>
/// Interpolated string handler for the spot-discovery trace.
/// The compiler asks this handler whether to append before it evaluates any
/// hole, so a disabled trace never formats an argument or allocates a string.
/// </summary>
[InterpolatedStringHandler]
internal ref struct ZLinkSpotDiscoveryLogMessage
{
    private DefaultInterpolatedStringHandler _inner;
    private readonly bool _enabled;

    public ZLinkSpotDiscoveryLogMessage(
        int literalLength,
        int formattedCount,
        out bool shouldAppend)
    {
        _enabled = ZLinkFrameworkDebugLog.SpotDiscoveryEnabled;
        shouldAppend = _enabled;
        _inner = _enabled
            ? new DefaultInterpolatedStringHandler(literalLength, formattedCount)
            : default;
    }

    public void AppendLiteral(string value)
    {
        if (_enabled) _inner.AppendLiteral(value);
    }

    public void AppendFormatted<T>(T value)
    {
        if (_enabled) _inner.AppendFormatted(value);
    }

    public void AppendFormatted<T>(T value, string? format)
    {
        if (_enabled) _inner.AppendFormatted(value, format);
    }

    public void AppendFormatted(string? value)
    {
        if (_enabled) _inner.AppendFormatted(value);
    }

    internal string ToStringAndClear()
    {
        return _enabled ? _inner.ToStringAndClear() : string.Empty;
    }
}
