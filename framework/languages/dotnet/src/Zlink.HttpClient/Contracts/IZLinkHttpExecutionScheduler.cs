namespace Zlink.HttpClient;

/// <summary>Captures the framework execution turn that owns a server-side HTTP call.</summary>
public interface IZLinkHttpExecutionScheduler
{
    /// <summary>
    ///     Captures the current framework execution turn. Returns <c>null</c> when the caller is not
    ///     running inside a framework turn.
    /// </summary>
    IZLinkHttpExecutionTurn? Capture();
}

/// <summary>
///     Provides the completion boundary for an HTTP call created inside a framework execution turn.
/// </summary>
public interface IZLinkHttpExecutionTurn
{
    /// <summary>
    ///     Runs an HTTP operation while releasing this turn, then queues its continuation on the
    ///     same execution line. Caller cancellation cancels the wait.
    /// </summary>
    ValueTask<TResult> YieldAsync<TResult>(
        Func<CancellationToken, ValueTask<TResult>> operation,
        CancellationToken cancellationToken = default);

    /// <summary>Queues a completion callback as a new turn on the captured execution line.</summary>
    void Post(Action callback);

    /// <summary>Reports a detached HTTP operation or callback failure to the framework error boundary.</summary>
    void ReportError(Exception exception);
}
