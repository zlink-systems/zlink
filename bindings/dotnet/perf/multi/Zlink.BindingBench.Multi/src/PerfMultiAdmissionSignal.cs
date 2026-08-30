using System.Diagnostics;

/// <summary>
///     Coalesces public async-admission completions into one wake-up for the
///     routed multi coordinator. Reply readiness is deliberately not part of
///     this signal: the coordinator drains POLLIN without blocking and waits
///     here only when every socket still owns a pending admission.
/// </summary>
internal sealed class PerfMultiAdmissionSignal
{
    private readonly object _sync = new();
    private readonly Action _signalAction;
    private TaskCompletionSource _availability = CreateAvailability();

    internal PerfMultiAdmissionSignal()
    {
        _signalAction = Signal;
    }

    internal void Track(Task admission)
    {
        ArgumentNullException.ThrowIfNull(admission);
        if (admission.IsCompleted)
        {
            Signal();
            return;
        }

        // Register a callback without allocating another continuation Task.
        // The tracked Task is already terminal before this callback runs, so a
        // woken coordinator can observe and retire the matching socket state.
        admission.ConfigureAwait(false).GetAwaiter()
            .UnsafeOnCompleted(_signalAction);
    }

    internal async ValueTask<bool> WaitAsync(long deadlineTicks)
    {
        TaskCompletionSource observed;
        lock (_sync)
            observed = _availability;

        long remainingTicks = deadlineTicks - Stopwatch.GetTimestamp();
        if (remainingTicks <= 0)
            return false;

        var timeout = TimeSpan.FromSeconds(
            remainingTicks / (double)Stopwatch.Frequency);
        try
        {
            await observed.Task.WaitAsync(timeout).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            return false;
        }

        lock (_sync)
        {
            if (ReferenceEquals(_availability, observed))
                _availability = CreateAvailability();
        }
        return true;
    }

    private void Signal()
    {
        TaskCompletionSource availability;
        lock (_sync)
            availability = _availability;
        availability.TrySetResult();
    }

    private static TaskCompletionSource CreateAvailability()
    {
        return new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
    }
}

/// <summary>
///     Bounds the post-measurement wait for public async admissions. A timeout
///     fails the client so its owning scope closes the sockets; Core then
///     completes any still-pending operations during socket shutdown.
/// </summary>
internal static class PerfMultiAdmissionDrain
{
    internal static async Task WaitAsync(IReadOnlyCollection<Task> admissions,
        int timeoutMs)
    {
        if (admissions.Count == 0)
            return;

        Task all = Task.WhenAll(admissions);
        try
        {
            await all.WaitAsync(TimeSpan.FromMilliseconds(
                Math.Max(1, timeoutMs))).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            // The owning client closes its sockets after this failure. Observe
            // a later terminal error so the abandoned aggregate cannot raise
            // an unobserved-task notification during a long benchmark run.
            _ = all.ContinueWith(static completed => _ = completed.Exception,
                CancellationToken.None,
                TaskContinuationOptions.OnlyOnFaulted
                | TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
            throw new TimeoutException(
                $"pending send admissions did not drain within {Math.Max(1, timeoutMs)} ms");
        }
    }
}
