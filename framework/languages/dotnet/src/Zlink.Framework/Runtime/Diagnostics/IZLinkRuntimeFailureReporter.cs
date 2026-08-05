namespace Zlink.Framework.Runtime.Diagnostics;

internal interface IZLinkRuntimeFailureReporter
{
    void ReportHandlerException(Exception exception);

    void ReportUnhandledCallbackException(Exception exception);

    void ReportRuntimeTaskException(
        string taskName,
        Exception exception);
}
