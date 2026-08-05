namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    internal ZLinkClientServerMonitoringState ClientServerMonitoringState(
        string channelName)
    {
        if (!Registration.Channels.TryGetValue(
                channelName,
                out var registration)
            || (!registration.HasClientServerClient
                && !registration.HasClientServerServer))
            throw new ZLinkConfigurationException(
                $"ClientServer channel '{channelName}' is not registered.");

        var state = GetOrStartState();
        lock (state.SyncRoot)
        {
            state.ClientServerClientRuntimes.TryGetValue(
                channelName,
                out var client);
            ZLinkClientServerServerIdentity? server = null;
            if (state.ClientServerServerBundles.TryGetValue(
                    channelName,
                    out var serverBundle))
                server = serverBundle.ClientServerServer;
            return new ZLinkClientServerMonitoringState(
                registration.HasClientServerClient,
                registration.HasClientServerServer,
                client,
                server);
        }
    }
}

internal sealed record ZLinkClientServerMonitoringState(
    bool HasClient,
    bool HasServer,
    ZLinkClientServerClientRuntime? Client,
    ZLinkClientServerServerIdentity? Server);
