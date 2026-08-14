namespace Zlink.Framework.Runtime.Handlers;

internal static class ZLinkHandlerInvocationEngine
{
    public static ValueTask<object?> InvokeAsync(
        object handler,
        ZLinkHandlerMethodInvoker invoker,
        IReadOnlyList<ZLinkHandlerArgumentKind> argumentPlan,
        object? message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        object? arg0 = null;
        object? arg1 = null;
        object? arg2 = null;
        object? arg3 = null;
        object? arg4 = null;
        for (var i = 0; i < argumentPlan.Count; i++)
        {
            if (i >= 5)
                throw new InvalidOperationException("Handler methods support at most five arguments.");

            var value = argumentPlan[i] switch
            {
                ZLinkHandlerArgumentKind.Message => message,
                ZLinkHandlerArgumentKind.Context => context,
                ZLinkHandlerArgumentKind.CancellationToken => cancellationToken,
                _ => null
            };

            switch (i)
            {
                case 0:
                    arg0 = value;
                    break;
                case 1:
                    arg1 = value;
                    break;
                case 2:
                    arg2 = value;
                    break;
                case 3:
                    arg3 = value;
                    break;
                case 4:
                    arg4 = value;
                    break;
            }
        }

        ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
        var result = invoker(handler, arg0, arg1, arg2, arg3, arg4);
        return ZLinkHandlerResultAwaiter.AwaitAsync(result);
    }

    public static ValueTask<object?> InvokeAsync(
        object handler,
        ZLinkHandlerMethodInvoker invoker,
        object? arg0 = null,
        object? arg1 = null,
        object? arg2 = null,
        object? arg3 = null,
        object? arg4 = null)
    {
        ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
        var result = invoker(handler, arg0, arg1, arg2, arg3, arg4);
        return ZLinkHandlerResultAwaiter.AwaitAsync(result);
    }
}
