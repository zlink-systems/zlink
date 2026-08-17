namespace Zlink.Framework.Runtime.Streams;

internal sealed record ZLinkStreamWireError(
    string? Code,
    string? Message)
{
    public static ZLinkStreamWireError FromException(Exception exception)
    {
        ArgumentNullException.ThrowIfNull(exception);
        // The stream error payload uses the cross-language snake_case wire
        // name (C++ stream_host_service stream_error_code).
        var code = exception is ZLinkFrameworkException frameworkException
            ? ZLinkErrorWireNames.Name(frameworkException.Kind)
            : exception.GetType().Name;
        return new ZLinkStreamWireError(code, exception.Message);
    }
}
