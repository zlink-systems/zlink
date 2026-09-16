using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Backend.DotNet.Adapters;

namespace Zlink.Framework.Runtime.Backend.DotNet;

internal sealed class ZLinkDotNetBackendAdapterFactory(
    ILoggerFactory? loggerFactory = null) : IZLinkBackendAdapterFactory
{
    private static readonly IZLinkMonitoringBackendAdapter
        MonitoringAdapter = new ZLinkDotNetMonitoringBackendAdapter();

    public IZLinkBackendRuntimeContext CreateRuntimeContext() =>
        new ZLinkDotNetBackendRuntimeContext(
            loggerFactory?.CreateLogger<ZLinkManagedMeshNode>());

    public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter()
    {
        return MonitoringAdapter;
    }
}
