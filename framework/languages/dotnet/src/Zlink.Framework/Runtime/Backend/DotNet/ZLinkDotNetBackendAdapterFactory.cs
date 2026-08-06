namespace Zlink.Framework.Runtime.Backend.DotNet;

internal sealed class ZLinkDotNetBackendAdapterFactory : IZLinkBackendAdapterFactory
{
    private static readonly IZLinkMonitoringBackendAdapter
        MonitoringAdapter = new ZLinkDotNetMonitoringBackendAdapter();

    public IZLinkBackendRuntimeContext CreateRuntimeContext() =>
        new ZLinkDotNetBackendRuntimeContext();

    public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter()
    {
        return MonitoringAdapter;
    }
}
