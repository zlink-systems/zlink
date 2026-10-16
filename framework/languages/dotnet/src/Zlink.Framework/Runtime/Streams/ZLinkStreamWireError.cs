using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Streams;

internal sealed record ZLinkStreamWireError(string? Code, string? Message)
{
    public static ZLinkStreamWireError FromException(Exception exception)
    {
        ArgumentNullException.ThrowIfNull(exception);
        var code = ZLinkErrorWireNames.Name(
            exception is ZLinkFrameworkException frameworkException
                ? frameworkException.Kind
                : ZLinkFrameworkErrorKind.InternalFailure
        );
        return new ZLinkStreamWireError(code, exception.Message);
    }
}
