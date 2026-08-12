using SubmitAdmission.Server.Infrastructure;
using SubmitAdmission.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;

namespace SubmitAdmission.Server;

internal sealed class NodeAdmissionHandler(
    ServerOptions options,
    HandlerGate gate,
    OperationEvidenceStore evidence)
    : IZLinkRouteSendHandler<AdmissionMsg>
{
    public async ValueTask HandleAsync(
        AdmissionMsg message,
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
    : IZLinkSendHandler<AdmissionMsg>
{
    public async ValueTask HandleAsync(
        AdmissionMsg message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        evidence.HandlerEntered(message.OperationId, "channel", options.Rid);
        await gate.WaitAsync(cancellationToken);
        evidence.HandlerCompleted(message.OperationId, "channel", options.Rid);
    }
}

internal sealed class NodeReadyHandler(ServerOptions options)
    : IZLinkRouteRequestHandler<RouteReadyReq, RouteReadyRes>
{
    public ValueTask<RouteReadyRes> HandleAsync(
        RouteReadyReq request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(new RouteReadyRes(options.Rid, request.Marker));
}

internal sealed class ChannelReadyHandler(ServerOptions options)
    : IZLinkRequestHandler<RouteReadyReq, RouteReadyRes>
{
    public ValueTask<RouteReadyRes> HandleAsync(
        RouteReadyReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(new RouteReadyRes(options.Rid, request.Marker));
}
