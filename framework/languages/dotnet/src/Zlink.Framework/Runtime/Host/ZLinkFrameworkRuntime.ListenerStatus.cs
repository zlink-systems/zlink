using Zlink.Framework.Runtime.Configuration;

namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    public ZLinkListenerStatus GetListenerStatus(ZLinkListenerKind kind, string name)
    {
        if (string.IsNullOrWhiteSpace(name))
            throw ListenerNotConfigured(kind, name);
        var state = _state;
        if (!IsStarted || state is null)
            throw ListenerNotConfigured(kind, name);

        string? endpoint;
        if (kind == ZLinkListenerKind.ClientServer)
        {
            var identity = AwaitStateLane(
                state.RunStateAsync(() =>
                    state.ClientServerServerBundles.TryGetValue(name, out var server)
                        ? server.ClientServerServer
                        : null
                )
            );
            endpoint = identity is null
                ? null
                : AwaitStateLane(identity.ReadAsync()).AdvertisedEndpoint;
        }
        else if (kind == ZLinkListenerKind.RouteMesh)
        {
            endpoint = RouteMeshEndpoint(state, name);
        }
        else
        {
            endpoint = AwaitStateLane(
                state.RunStateAsync(() =>
                    kind switch
                    {
                        ZLinkListenerKind.Fanout => state.PublisherBundles.TryGetValue(
                            name,
                            out var publisher
                        )
                            ? publisher.FanoutPublisher?.Endpoint
                            : null,
                        ZLinkListenerKind.Stream => state.StreamNodes.TryGetValue(
                            name,
                            out var stream
                        )
                            ? stream.AdvertisedEndpoint
                            : null,
                        _ => null,
                    }
                )
            );
        }
        if (string.IsNullOrWhiteSpace(endpoint))
            throw ListenerNotConfigured(kind, name);
        return new ZLinkListenerStatus(kind, name, endpoint, DateTimeOffset.UtcNow);
    }

    private string? RouteMeshEndpoint(ZLinkFrameworkComponentState state, string name)
    {
        if (
            !Registration.SpotNodes.TryGetValue(name, out var registration)
            || registration.Router is not { } router
        )
            return null;

        // The lane turn only reads the started node; the mesh status query and endpoint
        // resolution run outside the lane.
        var node = AwaitStateLane(
            state.RunStateAsync(() =>
                state.SpotNodes.TryGetValue(name, out var started) ? started : null
            )
        );
        if (node is null)
            return null;
        var bound = node.Node.MeshStatus().LocalEndpoint;
        if (string.IsNullOrWhiteSpace(bound))
            return null;
        return ZLinkNetworkEndpointResolver.Advertise(
            bound,
            router.AdvertiseHost,
            router.BindHost,
            Registration.NetworkOptions
        );
    }

    private static ZLinkFrameworkException ListenerNotConfigured(
        ZLinkListenerKind kind,
        string name
    ) => new(ZLinkFrameworkErrorKind.NotConfigured, $"{kind} listener '{name}' is not bound.");
}
