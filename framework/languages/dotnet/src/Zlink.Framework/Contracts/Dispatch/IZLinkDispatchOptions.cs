namespace Zlink.Framework.Contracts.Dispatch;

public interface IZLinkDispatchOptions
{
    IZLinkUnhandledDispatchOptions Unhandled { get; }

    IZLinkDiagnosticsOptions Diagnostics { get; }
}

public interface IZLinkUnhandledDispatchOptions
{
    ZLinkUnhandledDispatchAction Request { get; set; }

    ZLinkUnhandledDispatchAction Send { get; set; }

    ZLinkUnhandledDispatchAction Publish { get; set; }
}

public interface IZLinkDiagnosticsOptions
{
    IZLinkDiagnosticsOptions SetLevel(ZLinkDiagnosticsLevel level);

    IZLinkDiagnosticsOptions SetSampleRate(double rate);

    IZLinkDiagnosticsOptions IncludeMessageSizes(bool include);
}

public enum ZLinkDiagnosticsLevel
{
    Off = 0,
    Errors = 1,
    Normal = 2,
    Detailed = 3
}

public interface IZLinkDiagnosticsRuntime
{
    /// <summary>Gets or changes the process-wide diagnostics level synchronously.</summary>
    /// <remarks>
    /// Do not set this property from a framework execution context such as a handler or callback;
    /// use <see cref="SetLevelAsync" /> there. The setter is the blocking compatibility bridge over
    /// the asynchronous control surface.
    /// </remarks>
    ZLinkDiagnosticsLevel Level { get; set; }

    /// <summary>Changes the process-wide diagnostics level asynchronously.</summary>
    Task SetLevelAsync(ZLinkDiagnosticsLevel level);
}

public enum ZLinkUnhandledDispatchAction
{
    ReplyError = 0,
    LogAndDrop = 1,
    Drop = 2,
    Throw = 3
}
