namespace Zlink.Framework.Contracts.Configuration;

public enum ZLinkListenerKind
{
    RouteMesh,
    ClientServer,
    Fanout,
    Stream,
}

public sealed record ZLinkListenerStatus(
    ZLinkListenerKind Kind,
    string Name,
    string Endpoint,
    DateTimeOffset ObservedAt
);
