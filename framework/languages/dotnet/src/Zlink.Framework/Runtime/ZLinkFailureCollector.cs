namespace Zlink.Framework.Runtime;

/// <summary>
/// Preserves a primary failure while cleanup continues through every owned
/// resource. Callers add resources in ownership-specific teardown order.
/// </summary>
internal sealed class ZLinkFailureCollector(Exception? primaryFailure = null)
{
    private readonly List<Exception> _failures = primaryFailure is null ? [] : [primaryFailure];

    public async ValueTask CaptureAsync(Func<ValueTask> operation)
    {
        try
        {
            await operation().ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            _failures.Add(exception);
        }
    }

    public void Capture(Action operation)
    {
        try
        {
            operation();
        }
        catch (Exception exception)
        {
            _failures.Add(exception);
        }
    }

    public void ThrowIfAny()
    {
        var failure = BuildException();
        if (failure is not null)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failure).Throw();
    }

    public Exception? BuildException() => _failures.Count switch
    {
        0 => null,
        1 => _failures[0],
        _ => new AggregateException(_failures)
    };
}
