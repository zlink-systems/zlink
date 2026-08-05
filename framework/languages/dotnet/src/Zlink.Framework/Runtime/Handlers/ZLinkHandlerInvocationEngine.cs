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

        var result = invoker(handler, arg0, arg1, arg2, arg3, arg4);
        return ZLinkHandlerResultAwaiter.AwaitAsync(result);
    }

    public static ValueTask<object?> InvokeAsync(
        object handler,
        ZLinkHandlerMethodInvoker invoker,
        int argumentCount,
        Action<ZLinkHandlerArgumentSink> configureArguments)
    {
        object? arg0 = null;
        object? arg1 = null;
        object? arg2 = null;
        object? arg3 = null;
        object? arg4 = null;
        switch (argumentCount)
        {
            case 0:
                break;
            case 1:
                configureArguments(new ZLinkHandlerArgumentSink(
                    value => arg0 = value,
                    null,
                    null,
                    null,
                    null));
                break;
            case 2:
                configureArguments(new ZLinkHandlerArgumentSink(
                    value => arg0 = value,
                    value => arg1 = value,
                    null,
                    null,
                    null));
                break;
            case 3:
                configureArguments(new ZLinkHandlerArgumentSink(
                    value => arg0 = value,
                    value => arg1 = value,
                    value => arg2 = value,
                    null,
                    null));
                break;
            case 4:
                configureArguments(new ZLinkHandlerArgumentSink(
                    value => arg0 = value,
                    value => arg1 = value,
                    value => arg2 = value,
                    value => arg3 = value,
                    null));
                break;
            case 5:
                configureArguments(new ZLinkHandlerArgumentSink(
                    value => arg0 = value,
                    value => arg1 = value,
                    value => arg2 = value,
                    value => arg3 = value,
                    value => arg4 = value));
                break;
            default:
                throw new InvalidOperationException("Handler methods support at most five arguments.");
        }

        var result = invoker(handler, arg0, arg1, arg2, arg3, arg4);
        return ZLinkHandlerResultAwaiter.AwaitAsync(result);
    }
}

internal readonly struct ZLinkHandlerArgumentSink(
    Action<object?>? set0,
    Action<object?>? set1,
    Action<object?>? set2,
    Action<object?>? set3,
    Action<object?>? set4)
{
    public object? this[int index]
    {
        set
        {
            switch (index)
            {
                case 0:
                    set0?.Invoke(value);
                    break;
                case 1:
                    set1?.Invoke(value);
                    break;
                case 2:
                    set2?.Invoke(value);
                    break;
                case 3:
                    set3?.Invoke(value);
                    break;
                case 4:
                    set4?.Invoke(value);
                    break;
                default:
                    throw new ArgumentOutOfRangeException(nameof(index));
            }
        }
    }
}
