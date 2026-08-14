namespace Zlink.Framework.Runtime.Backend.Contracts;

/// <summary>
/// Owns the binding runtime context and creates the backend resources used by
/// one Framework runtime generation. The semantic runtime sees only backend
/// contracts; binding context options and shutdown ordering stay behind this
/// port.
/// </summary>
internal interface IZLinkBackendRuntimeContext : IAsyncDisposable
{
    void ConfigureCoreHwm(
        AutoHwmProfile profile,
        ulong memoryLimitBytes,
        ulong budgetBytes);

    CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot();

    void ResetCoreHwmBudgetMetrics();

    void ConfigureApplicationJobQueue(
        ZLinkApplicationJobQueue applicationJobQueue);

    IDealerSocket CreateDealerSocket();

    IRouterSocket CreateRouterSocket();

    IPubSocket CreatePublisherSocket();

    ISubSocket CreateSubscriberSocket();

    IZLinkBackendSpotNode CreateSpotNode(string meshName);

    IZLinkBackendStreamSocket CreateStreamSocket(
        string standaloneMeshName,
        IZLinkBackendSpotNode? actorDispatchNode = null);
}
