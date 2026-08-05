namespace Zlink.Framework.Runtime.Handlers;

internal enum ZLinkHandlerArgumentKind
{
    Message,
    Context,
    CancellationToken,
    Default
}

internal sealed record ZLinkHandlerEndpointDescriptor(
    ZLinkMessageKind Kind,
    string MessageName,
    Type DeclaringType,
    ZLinkHandlerMethodInvoker Invoker,
    IReadOnlyList<ZLinkHandlerArgumentKind> ArgumentPlan,
    Type MessageType,
    Type? ReplyType,
    Type? ContextType,
    bool HasCancellationToken,
    IReadOnlySet<string> Groups,
    string? ExplicitChannelName);

internal sealed record ZLinkRouteHandlerEndpointDescriptor(
    ZLinkMessageKind Kind,
    string MessageName,
    Type DeclaringType,
    ZLinkHandlerMethodInvoker Invoker,
    Type MessageType,
    Type? ReplyType,
    IReadOnlySet<string> Groups);

internal enum ZLinkHandlerEndpointSurface
{
    Channel,
    Route
}

internal readonly record struct ZLinkHandlerGroupCatalogEntry(
    ZLinkHandlerEndpointSurface Surface,
    ZLinkMessageKind Kind);

internal readonly record struct ZLinkHandlerSelectionKey(
    ZLinkMessageKind Kind,
    string ChannelName,
    string MessageName);