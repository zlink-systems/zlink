namespace Zlink.Framework.Runtime.Backend.Contracts;

internal interface IZLinkBackendAdapterFactory
{
    IZLinkChannelBackendAdapter CreateChannelAdapter();

    IZLinkSpotBackendAdapter CreateSpotAdapter();

    IZLinkStreamBackendAdapter CreateStreamAdapter();

    IZLinkMonitoringBackendAdapter CreateMonitoringAdapter();
}
