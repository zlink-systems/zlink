using System.Reflection;

namespace Zlink.Framework.Runtime.Handlers;

internal static class ZLinkHandlerMethodShape
{
    public static ParameterInfo[] RequireParameterCount(
        Type handlerType,
        MethodInfo method,
        int expectedCount,
        string description)
    {
        var parameters = method.GetParameters();
        if (parameters.Length != expectedCount)
            throw new InvalidOperationException(
                $"{description} '{handlerType}' method '{method.Name}' must declare exactly {expectedCount} parameters.");

        return parameters;
    }

    public static void RequireCancellationToken(
        Type handlerType,
        MethodInfo method,
        ParameterInfo parameter,
        string description,
        string position = "last")
    {
        if (parameter.ParameterType != typeof(CancellationToken))
            throw new InvalidOperationException(
                $"{description} '{handlerType}' method '{method.Name}' must use CancellationToken as the {position} parameter.");
    }

    public static Type RequireReplyType(Type returnType, string description)
    {
        if (returnType.IsGenericType
            && (returnType.GetGenericTypeDefinition() == typeof(ValueTask<>)
                || returnType.GetGenericTypeDefinition() == typeof(Task<>)))
            return returnType.GetGenericArguments()[0];

        if (returnType == typeof(ValueTask)
            || returnType == typeof(Task)
            || returnType == typeof(void))
            throw new InvalidOperationException($"{description} must return a reply value.");

        return returnType;
    }

    public static void RequireNoReply(Type handlerType, MethodInfo method, string description)
    {
        var returnType = method.ReturnType;
        if (returnType == typeof(void)
            || returnType == typeof(ValueTask)
            || returnType == typeof(Task))
            return;

        throw new InvalidOperationException(
            $"{description} '{handlerType}' method '{method.Name}' must not return a reply value.");
    }
}