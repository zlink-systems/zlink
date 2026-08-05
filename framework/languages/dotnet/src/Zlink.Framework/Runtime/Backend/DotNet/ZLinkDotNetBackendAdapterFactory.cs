namespace Zlink.Framework.Runtime.Backend.DotNet;

internal sealed class ZLinkDotNetBackendAdapterFactory : IZLinkBackendAdapterFactory
{
    private static readonly IZLinkChannelBackendAdapter ChannelAdapter = new ZLinkDotNetChannelBackendAdapter();
    private static readonly IZLinkSpotBackendAdapter SpotAdapter = new ZLinkDotNetSpotBackendAdapter();
    private static readonly IZLinkStreamBackendAdapter StreamAdapter = new ZLinkDotNetStreamBackendAdapter();

    private static readonly IZLinkMonitoringBackendAdapter
        MonitoringAdapter = new ZLinkDotNetMonitoringBackendAdapter();

    public IZLinkChannelBackendAdapter CreateChannelAdapter()
    {
        return ChannelAdapter;
    }

    public IZLinkSpotBackendAdapter CreateSpotAdapter()
    {
        return SpotAdapter;
    }

    public IZLinkStreamBackendAdapter CreateStreamAdapter()
    {
        return StreamAdapter;
    }

    public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter()
    {
        return MonitoringAdapter;
    }
}
