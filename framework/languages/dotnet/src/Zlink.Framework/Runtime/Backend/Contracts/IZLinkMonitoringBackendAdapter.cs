namespace Zlink.Framework.Runtime.Backend.Contracts;

internal interface IZLinkMonitoringBackendAdapter
{
    IZLinkBackendSocketMonitor OpenSocketMonitor(IAsyncDisposable socket);
}
