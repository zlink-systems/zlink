namespace Zlink.Framework.Runtime.Streams;

internal sealed record ZLinkStreamWireError(
    string? Code,
    string? Message)
{
    public static ZLinkStreamWireError FromException(Exception exception)
    {
        ArgumentNullException.ThrowIfNull(exception);
        var code = exception is ZLinkFrameworkException frameworkException
            ? frameworkException.Kind.ToString()
            : exception.GetType().Name;
        return new ZLinkStreamWireError(code, exception.Message);
    }
}
