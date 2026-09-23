using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Streams;

internal sealed record ZLinkStreamWireError(string? Code, string? Message)
{
    public static ZLinkStreamWireError FromException(Exception exception)
    {
        ArgumentNullException.ThrowIfNull(exception);
        var code = exception is ZLinkFrameworkException frameworkException
            ? ZLinkErrorWireNames.Name(frameworkException.Kind)
            : exception.GetType().Name;
        return new ZLinkStreamWireError(code, exception.Message);
    }
}
