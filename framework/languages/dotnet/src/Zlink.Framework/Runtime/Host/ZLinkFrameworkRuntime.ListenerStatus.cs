namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    public ZLinkListenerStatus GetListenerStatus(ZLinkListenerKind kind, string name)
    {
        var endpoint = AwaitStateLane(ListenerRecords.ReadAsync(kind, name));
        if (endpoint is null)
            throw ListenerNotConfigured(kind, name);
        return new ZLinkListenerStatus(kind, name, endpoint, DateTimeOffset.UtcNow);
    }

    private static ZLinkFrameworkException ListenerNotConfigured(
        ZLinkListenerKind kind,
        string name
    ) => new(ZLinkFrameworkErrorKind.NotConfigured, $"{kind} listener '{name}' is not bound.");
}
