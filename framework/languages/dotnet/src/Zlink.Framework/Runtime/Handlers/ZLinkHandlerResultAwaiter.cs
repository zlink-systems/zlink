using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Handlers;

internal static class ZLinkHandlerResultAwaiter
{
    private static readonly ConcurrentDictionary<Type, Func<object, ValueTask<object?>>> GenericAwaiters = new();

    public static ValueTask<object?> AwaitAsync(object? result)
    {
        if (result is null) return new ValueTask<object?>((object?)null);

        switch (result)
        {
            case Task task when result.GetType() == typeof(Task):
                return task.IsCompletedSuccessfully
                    ? new ValueTask<object?>((object?)null)
                    : AwaitTaskAsync(task);
            case ValueTask valueTask:
                return valueTask.IsCompletedSuccessfully
                    ? new ValueTask<object?>((object?)null)
                    : AwaitValueTaskAsync(valueTask);
        }

        return IsGenericAwaitable(result.GetType())
            ? GenericAwaiters.GetOrAdd(result.GetType(), CreateGenericAwaiter)(result)
            : new ValueTask<object?>(result);
    }

    private static bool IsGenericAwaitable(Type resultType)
    {
        return resultType.IsGenericType
               && (resultType.GetGenericTypeDefinition() == typeof(Task<>)
                   || resultType.GetGenericTypeDefinition() == typeof(ValueTask<>));
    }

    private static Func<object, ValueTask<object?>> CreateGenericAwaiter(Type resultType)
    {
        var resultValueType = resultType.GetGenericArguments()[0];
        var methodName = resultType.GetGenericTypeDefinition() == typeof(Task<>)
            ? nameof(AwaitTaskAsync)
            : nameof(AwaitValueTaskAsync);
        var method = typeof(ZLinkHandlerResultAwaiter)
            .GetMethod(methodName)!
            .MakeGenericMethod(resultValueType);
        return (Func<object, ValueTask<object?>>)method.CreateDelegate(typeof(Func<object, ValueTask<object?>>));
    }

    public static ValueTask<object?> AwaitTaskAsync<T>(object result)
    {
        var task = (Task<T>)result;
        return task.IsCompletedSuccessfully
            ? new ValueTask<object?>(task.Result)
            : AwaitTaskSlowAsync(task);
    }

    public static ValueTask<object?> AwaitValueTaskAsync<T>(object result)
    {
        var valueTask = (ValueTask<T>)result;
        return valueTask.IsCompletedSuccessfully
            ? new ValueTask<object?>(valueTask.Result)
            : AwaitValueTaskSlowAsync(valueTask);
    }

    private static async ValueTask<object?> AwaitTaskAsync(Task task)
    {
        await task.ConfigureAwait(false);
        return null;
    }

    private static async ValueTask<object?> AwaitValueTaskAsync(ValueTask valueTask)
    {
        await valueTask.ConfigureAwait(false);
        return null;
    }

    private static async ValueTask<object?> AwaitTaskSlowAsync<T>(Task<T> task)
    {
        return await task.ConfigureAwait(false);
    }

    private static async ValueTask<object?> AwaitValueTaskSlowAsync<T>(ValueTask<T> valueTask)
    {
        return await valueTask.ConfigureAwait(false);
    }
}
