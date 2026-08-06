namespace Zlink.Framework.Runtime.Backend.Contracts;

internal interface IZLinkBackendAdapterFactory
{
    IZLinkBackendRuntimeContext CreateRuntimeContext();

    IZLinkMonitoringBackendAdapter CreateMonitoringAdapter();
}
