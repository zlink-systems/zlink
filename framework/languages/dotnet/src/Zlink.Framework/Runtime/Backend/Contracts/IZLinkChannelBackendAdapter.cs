namespace Zlink.Framework.Runtime.Backend.Contracts;

internal interface IZLinkChannelBackendAdapter
{
    IZLinkBackendContext CreateContext();

    void ConfigureAutoHwm(
        IZLinkBackendContext context,
        ZLinkApplicationHwmProfile profile)
    {
    }

    IZLinkBackendDealerSocket CreateDealerSocket(IZLinkBackendContext context);

    IZLinkBackendRouterSocket CreateRouterSocket(IZLinkBackendContext context);

    IZLinkBackendPublisherSocket CreatePublisherSocket(IZLinkBackendContext context);

    IZLinkBackendSubscriberSocket CreateSubscriberSocket(IZLinkBackendContext context);
}
