using System.Reflection;

namespace Zlink.Framework.Runtime.Handlers;

internal static class ZLinkHandlerContractDescriptorSupport
{
    public static ZLinkHandlerMethodInvoker CreateHandleAsyncInvoker(
        Type handlerType,
        string description)
    {
        var method = handlerType.GetMethod(
                         nameof(IZLinkSendHandler<object>.HandleAsync),
                         BindingFlags.Instance | BindingFlags.Public)
                     ?? throw new InvalidOperationException(
                         $"{description} '{handlerType}' does not expose HandleAsync.");
        return ZLinkHandlerMethodInvokerFactory.Create(method);
    }

    public static void RequireExactType(
        Type handlerType,
        Type expectedType,
        Type actualType,
        string description)
    {
        if (actualType != expectedType)
            throw new InvalidOperationException(
                $"{description} '{handlerType}' targets '{actualType}', but registration expects '{expectedType}'.");
    }

    public static void RequireAssignableFrom(
        Type handlerType,
        Type expectedRuntimeType,
        Type declaredType,
        string description)
    {
        if (!declaredType.IsAssignableFrom(expectedRuntimeType))
            throw new InvalidOperationException(
                $"{description} '{handlerType}' targets '{declaredType}', but runtime type is '{expectedRuntimeType}'.");
    }
}