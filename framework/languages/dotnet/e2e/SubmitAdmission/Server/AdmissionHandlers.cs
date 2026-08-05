using SubmitAdmission.Server.Infrastructure;
using SubmitAdmission.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;

namespace SubmitAdmission.Server;

internal sealed class NodeAdmissionHandler(
    ServerOptions options,
    HandlerGate gate,
    OperationEvidenceStore evidence)
    : IZLinkRouteSendHandler<AdmissionMessage>
{
    public async ValueTask HandleAsync(
        AdmissionMessage message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        if (!context.Metadata.Values.TryGetValue("scenario", out var operationId)
            || !string.Equals(operationId, message.OperationId, StringComparison.Ordinal))
            throw new InvalidOperationException("RID-direct metadata did not use the normal dispatch pipeline.");
        evidence.HandlerEntered(message.OperationId, "node-direct", options.Rid);
        await gate.WaitAsync(cancellationToken);
        evidence.HandlerCompleted(message.OperationId, "node-direct", options.Rid);
    }
}

internal sealed class ChannelAdmissionHandler(
    ServerOptions options,
    HandlerGate gate,
    OperationEvidenceStore evidence)
    : IZLinkSendHandler<AdmissionMessage>
{
    public async ValueTask HandleAsync(
        AdmissionMessage message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        evidence.HandlerEntered(message.OperationId, "channel", options.Rid);
        await gate.WaitAsync(cancellationToken);
        evidence.HandlerCompleted(message.OperationId, "channel", options.Rid);
    }
}

internal sealed class NodeReadyHandler(ServerOptions options)
    : IZLinkRouteRequestHandler<RouteReadyRequest, RouteReadyReply>
{
    public ValueTask<RouteReadyReply> HandleAsync(
        RouteReadyRequest request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(new RouteReadyReply(options.Rid, request.Marker));
}

internal sealed class ChannelReadyHandler(ServerOptions options)
    : IZLinkRequestHandler<RouteReadyRequest, RouteReadyReply>
{
    public ValueTask<RouteReadyReply> HandleAsync(
        RouteReadyRequest request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(new RouteReadyReply(options.Rid, request.Marker));
}
