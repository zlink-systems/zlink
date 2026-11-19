using System.Linq.Expressions;

namespace Zlink.Framework.Runtime.Handlers;

internal static class ZLinkHandlerResultAwaiter
{
    // The declared return type determines the adapter once, while the invoker is
    // compiled. ValueTask results stay value types through the dispatch boundary.
    internal static Expression Prepare(Expression result)
    {
        var type = result.Type;
        if (type == typeof(void))
            return Expression.Block(result, Expression.Default(typeof(ValueTask<object?>)));
        if (type == typeof(Task) || type == typeof(ValueTask))
            return Expression.Call(
                typeof(ZLinkHandlerResultAwaiter),
                type == typeof(Task) ? nameof(AwaitTaskAsync) : nameof(AwaitValueTaskAsync),
                Type.EmptyTypes,
                result
            );
        if (type.IsGenericType)
        {
            var definition = type.GetGenericTypeDefinition();
            if (definition == typeof(Task<>) || definition == typeof(ValueTask<>))
                return Expression.Call(
                    typeof(ZLinkHandlerResultAwaiter),
                    definition == typeof(Task<>)
                        ? nameof(AwaitTaskAsync)
                        : nameof(AwaitValueTaskAsync),
                    type.GetGenericArguments(),
                    result
                );
        }
        return Expression.New(
            typeof(ValueTask<object?>).GetConstructor([typeof(object)])!,
            Expression.Convert(result, typeof(object))
        );
    }

    public static ValueTask<object?> AwaitTaskAsync<T>(Task<T> task) =>
        task.IsCompletedSuccessfully
            ? new ValueTask<object?>(task.Result)
            : AwaitTaskSlowAsync(task);

    public static ValueTask<object?> AwaitValueTaskAsync<T>(ValueTask<T> task) =>
        task.IsCompletedSuccessfully
            ? new ValueTask<object?>(task.Result)
            : AwaitValueTaskSlowAsync(task);

    public static async ValueTask<object?> AwaitTaskAsync(Task? task)
    {
        if (task is not null)
            await task.ConfigureAwait(false);
        return null;
    }

    public static async ValueTask<object?> AwaitValueTaskAsync(ValueTask task)
    {
        await task.ConfigureAwait(false);
        return null;
    }

    private static async ValueTask<object?> AwaitTaskSlowAsync<T>(Task<T> task) =>
        await task.ConfigureAwait(false);

    private static async ValueTask<object?> AwaitValueTaskSlowAsync<T>(ValueTask<T> task) =>
        await task.ConfigureAwait(false);
}
