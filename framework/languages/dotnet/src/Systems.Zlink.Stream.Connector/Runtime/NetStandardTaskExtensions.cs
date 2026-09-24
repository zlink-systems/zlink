#if NETSTANDARD2_1
namespace System.Threading.Tasks;

internal static class NetStandardTaskExtensions
{
    public static async Task WaitAsync(this Task task, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var cancelled = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var registration = cancellationToken.Register(() => cancelled.TrySetResult(true));
        if (await Task.WhenAny(task, cancelled.Task).ConfigureAwait(false) != task)
            cancellationToken.ThrowIfCancellationRequested();
        await task.ConfigureAwait(false);
    }

    public static async Task<T> WaitAsync<T>(this Task<T> task, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var cancelled = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var registration = cancellationToken.Register(() => cancelled.TrySetResult(true));
        if (await Task.WhenAny(task, cancelled.Task).ConfigureAwait(false) != task)
            cancellationToken.ThrowIfCancellationRequested();
        return await task.ConfigureAwait(false);
    }
}
#endif
