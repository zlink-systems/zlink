using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.UnitTests.Runtime;

internal sealed class AuditRuntimeFailureReporter : IZLinkRuntimeFailureReporter
{
    internal System.Collections.Concurrent.ConcurrentQueue<Exception> Failures { get; } = new();

    public void ReportHandlerException(Exception exception) => Failures.Enqueue(exception);

    public void ReportUnhandledCallbackException(Exception exception) =>
        Failures.Enqueue(exception);

    public void ReportRuntimeTaskException(string taskName, Exception exception) =>
        Failures.Enqueue(exception);
}
