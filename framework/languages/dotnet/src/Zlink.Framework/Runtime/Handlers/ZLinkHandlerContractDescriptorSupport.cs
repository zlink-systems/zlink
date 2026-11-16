using System.Reflection;

namespace Zlink.Framework.Runtime.Handlers;

internal static class ZLinkHandlerContractDescriptorSupport
{
    internal static readonly IReadOnlyList<ZLinkHandlerArgumentKind> HandleAsyncArgumentPlan =
    [
        ZLinkHandlerArgumentKind.Message,
        ZLinkHandlerArgumentKind.Context,
        ZLinkHandlerArgumentKind.CancellationToken,
    ];

    public static ZLinkHandlerMethodInvoker CreateHandleAsyncInvoker(
        Type handlerType,
        string description
    )
    {
        var method =
            handlerType.GetMethod(
                nameof(IZLinkSendHandler<object>.HandleAsync),
                BindingFlags.Instance | BindingFlags.Public
            )
            ?? throw new InvalidOperationException(
                $"{description} '{handlerType}' does not expose HandleAsync."
            );
        return ZLinkHandlerMethodInvokerFactory.Create(method);
    }

    public static void RequireExactType(
        Type handlerType,
        Type expectedType,
        Type actualType,
        string description
    )
    {
        if (actualType != expectedType)
            throw new ZLinkConfigurationException(
                $"{description} '{handlerType}' targets '{actualType}', but registration expects '{expectedType}'."
            );
    }
}
