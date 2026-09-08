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

    internal static int RemainingTimeoutMilliseconds(long deadlineTicks)
    {
        long remainingTicks = deadlineTicks - Stopwatch.GetTimestamp();
        if (remainingTicks <= 0)
            return 0;
        long remainingMs = (long)Math.Ceiling(
            remainingTicks * 1000.0 / Stopwatch.Frequency);
        return remainingMs > int.MaxValue ? int.MaxValue : (int)remainingMs;
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
///     Tracks submitted echo records until their matching replies are received.
///     The count is observed only while draining after the active window; it
///     never controls whether the active loop may submit another record.
/// </summary>
internal sealed class PerfMultiEchoReplyDrain
{
    private long _pending;

    internal long Pending => Volatile.Read(ref _pending);

    // Count before starting admission because receive dispatch can precede the
    // scheduled continuation which observes a successful async admission.
    internal void Submitted()
    {
        Interlocked.Increment(ref _pending);
    }

    internal void AdmissionRejected()
    {
        FinishOne("echo admission rejection without a matching submission");
    }

    internal void Received()
    {
        FinishOne("echo reply without a matching submission");
    }

    internal async Task WaitAsync(long deadlineTicks,
        PerfMultiAdmissionSignal admissionSignal,
        Action completeAdmissions, Func<bool> hasPendingAdmissions,
        Func<int, int> poll, Action<int> dispatch)
    {
        while (true)
        {
            completeAdmissions();
            bool admissionsPending = hasPendingAdmissions();
            if (!admissionsPending && Pending == 0)
                return;

            int timeoutMs =
                PerfMultiAdmissionSignal.RemainingTimeoutMilliseconds(
                    deadlineTicks);
            if (timeoutMs <= 0)
                throw DrainTimeout(admissionsPending);

            // If every echo has already arrived, only an admission continuation
            // remains. Otherwise keep receiving replies while each socket's
            // binding runtime continues its async admission independently.
            if (Pending == 0)
            {
                if (!await admissionSignal.WaitAsync(deadlineTicks)
                        .ConfigureAwait(false))
                    throw DrainTimeout(hasPendingAdmissions());
                continue;
            }

            int readyCount = poll(timeoutMs);
            if (readyCount > 0)
                dispatch(readyCount);
        }
    }

    internal static long DeadlineAfter(long activeDeadlineTicks, int timeoutMs)
    {
        long milliseconds = Math.Max(1, timeoutMs);
        long deltaTicks = (milliseconds * Stopwatch.Frequency + 999) / 1000;
        return activeDeadlineTicks + Math.Max(1, deltaTicks);
    }

    private void FinishOne(string errorMessage)
    {
        while (true)
        {
            long pending = Volatile.Read(ref _pending);
            if (pending == 0)
                throw new InvalidOperationException(errorMessage);
            if (Interlocked.CompareExchange(ref _pending, pending - 1,
                    pending) == pending)
                return;
        }
    }

    private TimeoutException DrainTimeout(bool admissionsPending)
    {
        return new TimeoutException(
            $"pending send admissions or admitted echoes did not drain "
            + $"(echoes={Pending}, admissions={admissionsPending})");
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
