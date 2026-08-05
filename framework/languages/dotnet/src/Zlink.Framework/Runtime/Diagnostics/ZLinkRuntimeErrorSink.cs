namespace Zlink.Framework.Runtime.Diagnostics;

internal sealed class ZLinkRuntimeErrorSink : IZLinkRuntimeFailureReporter, IDisposable
{
    private readonly Action<Exception> _processFailure;
    private Action<Exception>? _unhandledCallbackException;
    private int _disposed;

    public ZLinkRuntimeErrorSink(
        Action<Exception>? processFailure = null)
    {
        _processFailure = processFailure ?? ReportProcessFailure;
    }

    public event Action<Exception> UnhandledCallbackException
    {
        add
        {
            if (Volatile.Read(ref _disposed) == 0) _unhandledCallbackException += value;
        }
        remove => _unhandledCallbackException -= value;
    }

    public void ReportHandlerException(Exception exception)
    {
        ReportUnhandledCallbackException(exception);
    }

    public void ReportRuntimeTaskException(
        string taskName,
        Exception exception)
    {
        ZLinkFrameworkDebugLog.TaskFailure(taskName, exception);
        NotifyUnhandledCallbackException(exception);
    }

    public void ReportUnhandledCallbackException(Exception exception)
    {
        if (Volatile.Read(ref _disposed) != 0) return;
        ZLinkFrameworkDebugLog.UnhandledCallbackFailure(exception);
        NotifyUnhandledCallbackException(exception);
    }

    public Action<Exception> CaptureGenerationReporter()
    {
        var callback = _unhandledCallbackException;
        return exception =>
        {
            ZLinkFrameworkDebugLog.UnhandledCallbackFailure(exception);
            try
            {
                _processFailure(exception);
            }
            catch
            {
            }
            try
            {
                callback?.Invoke(exception);
            }
            catch
            {
            }
        };
    }

    private static void ReportProcessFailure(Exception exception) =>
        System.Diagnostics.Trace.TraceError(
            "ZLink detached generation cleanup failed: {0}",
            exception);

    private void NotifyUnhandledCallbackException(Exception exception)
    {
        if (Volatile.Read(ref _disposed) != 0) return;
        try
        {
            _unhandledCallbackException?.Invoke(exception);
        }
        catch
        {
        }
    }

    public void Dispose()
    {
        Interlocked.Exchange(ref _disposed, 1);
        _unhandledCallbackException = null;
    }
}
