using System.Collections.Concurrent;
using System.Linq.Expressions;
using System.Reflection;

namespace Zlink.Framework.Runtime.Handlers;

internal delegate object? ZLinkHandlerMethodInvoker(
    object target,
    object? arg0,
    object? arg1,
    object? arg2,
    object? arg3,
    object? arg4);

internal static class ZLinkHandlerMethodInvokerFactory
{
    private static readonly ConcurrentDictionary<MethodInfo, ZLinkHandlerMethodInvoker> Cache = new();

    public static ZLinkHandlerMethodInvoker Create(MethodInfo method)
    {
        ArgumentNullException.ThrowIfNull(method);
        return Cache.GetOrAdd(method, Compile);
    }

    private static ZLinkHandlerMethodInvoker Compile(MethodInfo method)
    {
        if (method.ContainsGenericParameters)
            throw new ZLinkConfigurationException(
                $"Handler method '{method.DeclaringType?.FullName}.{method.Name}' must not contain open generic parameters.");

        var parameters = method.GetParameters();
        foreach (var parameter in parameters)
            if (parameter.ParameterType.IsByRef)
                throw new ZLinkConfigurationException(
                    $"Handler method '{method.DeclaringType?.FullName}.{method.Name}' must not use ref, in, or out parameters.");

        var target = Expression.Parameter(typeof(object), "target");
        var arg0 = Expression.Parameter(typeof(object), "arg0");
        var arg1 = Expression.Parameter(typeof(object), "arg1");
        var arg2 = Expression.Parameter(typeof(object), "arg2");
        var arg3 = Expression.Parameter(typeof(object), "arg3");
        var arg4 = Expression.Parameter(typeof(object), "arg4");
        var argumentExpressions = new[] { arg0, arg1, arg2, arg3, arg4 };

        var convertedArguments = new Expression[parameters.Length];
        for (var i = 0; i < parameters.Length; i++)
        {
            if (i >= argumentExpressions.Length)
                throw new ZLinkConfigurationException(
                    $"Handler method '{method.DeclaringType?.FullName}.{method.Name}' has too many parameters.");
            var argument = argumentExpressions[i];
            convertedArguments[i] = Expression.Convert(argument, parameters[i].ParameterType);
        }

        var instance = method.IsStatic
            ? null
            : Expression.Convert(target, method.DeclaringType!);
        var call = Expression.Call(instance, method, convertedArguments);

        Expression body = method.ReturnType == typeof(void)
            ? Expression.Block(call, Expression.Constant(null, typeof(object)))
            : Expression.Convert(call, typeof(object));

        return Expression
            .Lambda<ZLinkHandlerMethodInvoker>(body, target, arg0, arg1, arg2, arg3, arg4)
            .Compile();
    }
}
