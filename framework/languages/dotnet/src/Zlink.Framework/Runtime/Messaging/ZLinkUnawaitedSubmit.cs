namespace Zlink.Framework.Runtime.Messaging;

/// <summary>
/// Observes fire-and-forget submit tasks. A Submit() caller opted out of
/// awaiting the result, not out of the failure existing: a submit that dies
/// locally (unroutable target, stopped socket) must still reach monitoring,
/// or stale-address sends vanish without a trace (spot-address messaging
/// spec §4). Cancellation is not a failure.
/// </summary>
internal static class ZLinkUnawaitedSubmit
{
    public static void Observe(
        ValueTask task,
        string operationName,
        IZLinkRuntimeFailureReporter errorSink)
    {
        if (task.IsCompletedSuccessfully) return;

        _ = ObserveAsync(task, operationName, errorSink);
    }

    private static async Task ObserveAsync(
        ValueTask task,
        string operationName,
        IZLinkRuntimeFailureReporter errorSink)
    {
        try
        {
            await task.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception exception)
        {
            errorSink.ReportRuntimeTaskException(operationName, exception);
        }
    }
}
