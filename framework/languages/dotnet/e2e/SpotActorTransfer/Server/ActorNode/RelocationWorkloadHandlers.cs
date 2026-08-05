using SpotActorTransfer.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace SpotActorTransfer.ActorNode;

internal static class RelocationWorkloadHandlers
{
    internal static RelocationWorkloadReply Reply(
        string targetId,
        string nodeRid,
        ulong objectGeneration,
        RelocationWorkloadRequest request,
        EvidenceStore evidence,
        string? observedOperationId,
        string? correlationId)
    {
        var now =
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var operationId =
            observedOperationId ?? request.OperationId;
        var withinDeadline =
            now <= request.AbsoluteDeadlineUnixTimeMilliseconds;
        evidence.Add(
            request.Scenario,
            targetId,
            "workload_request",
            $"sequence={request.Sequence};operation={operationId};"
            + $"correlation={correlationId ?? "unavailable"};"
            + $"owner={nodeRid};"
            + $"generation={objectGeneration};"
            + $"withinDeadline={withinDeadline}");
        return new RelocationWorkloadReply(
            request.Scenario,
            request.Sequence,
            operationId,
            correlationId,
            targetId,
            nodeRid,
            checked((long)objectGeneration),
            withinDeadline,
            now);
    }

    internal static void Accept(
        string targetId,
        string nodeRid,
        ulong objectGeneration,
        RelocationWorkloadPacket packet,
        EvidenceStore evidence,
        string? observedOperationId)
    {
        var operationId =
            observedOperationId ?? packet.OperationId;
        evidence.Add(
            packet.Scenario,
            targetId,
            "workload_one_way",
            $"sequence={packet.Sequence};operation={operationId};"
            + $"owner={nodeRid};"
            + $"generation={objectGeneration};withinDeadline="
            + (DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
               <= packet.AbsoluteDeadlineUnixTimeMilliseconds));
    }
}

internal sealed class EntryWorkloadRequestHandler(EvidenceStore evidence)
    : IZLinkEntrySpotActorRequestHandler<
        TransferEntrySpot,
        TransferActor,
        RelocationWorkloadRequest,
        RelocationWorkloadReply>
{
    public ValueTask<RelocationWorkloadReply> HandleAsync(
        TransferEntrySpot spot,
        TransferActor actor,
        IZLinkMessageContext context,
        RelocationWorkloadRequest request,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(RelocationWorkloadHandlers.Reply(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            actor.Context.ObjectGeneration,
            request,
            evidence,
            context.Metadata.Find(
                RelocationWorkloadMetadata.OperationId),
            context.CorrelationId));
    }
}

internal sealed class EntryRelocationQueueBlockHandler(
    EvidenceStore evidence,
    TransferGateStore gates)
    : IZLinkEntrySpotActorRequestHandler<
        TransferEntrySpot,
        TransferActor,
        RelocationQueueBlockReq,
        RelocationQueueBlockRes>
{
    public async ValueTask<RelocationQueueBlockRes> HandleAsync(
        TransferEntrySpot spot,
        TransferActor actor,
        IZLinkMessageContext context,
        RelocationQueueBlockReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        evidence.Add(
            request.Scenario,
            actor.ActorId,
            "queue_block_started",
            spot.Context.NodeRid.ToString());
        await gates.WaitAsync(request.TargetId, cancellationToken)
            .ConfigureAwait(false);
        evidence.Add(
            request.Scenario,
            actor.ActorId,
            "queue_block_released",
            spot.Context.NodeRid.ToString());
        return new RelocationQueueBlockRes(
            actor.ActorId,
            spot.Context.NodeRid.ToString());
    }
}

internal sealed class EntryWorkloadSendHandler(EvidenceStore evidence)
    : IZLinkEntrySpotActorSendHandler<
        TransferEntrySpot,
        TransferActor,
        RelocationWorkloadPacket>
{
    public ValueTask HandleAsync(
        TransferEntrySpot spot,
        TransferActor actor,
        IZLinkMessageContext context,
        RelocationWorkloadPacket message,
        CancellationToken cancellationToken)
    {
        _ = spot;
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        RelocationWorkloadHandlers.Accept(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            actor.Context.ObjectGeneration,
            message,
            evidence,
            context.Metadata.Find(
                RelocationWorkloadMetadata.OperationId));
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotActorRequestHandler(nameof(RelocationWorkloadRequest))]
internal sealed class UserSpotActorWorkloadRequestHandler(EvidenceStore evidence)
    : IZLinkSpotActorRequestHandler<
        TransferUserSpot,
        TransferActor,
        RelocationWorkloadRequest,
        RelocationWorkloadReply>
{
    public ValueTask<RelocationWorkloadReply> HandleAsync(
        TransferUserSpot spot,
        TransferActor actor,
        IZLinkMessageContext context,
        RelocationWorkloadRequest request,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(RelocationWorkloadHandlers.Reply(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            actor.Context.ObjectGeneration,
            request,
            evidence,
            context.Metadata.Find(
                RelocationWorkloadMetadata.OperationId),
            context.CorrelationId));
    }
}

[ZLinkSpotActorSendHandler(nameof(RelocationWorkloadPacket))]
internal sealed class UserSpotActorWorkloadSendHandler(EvidenceStore evidence)
    : IZLinkSpotActorSendHandler<
        TransferUserSpot,
        TransferActor,
        RelocationWorkloadPacket>
{
    public ValueTask HandleAsync(
        TransferUserSpot spot,
        TransferActor actor,
        IZLinkMessageContext context,
        RelocationWorkloadPacket message,
        CancellationToken cancellationToken)
    {
        _ = spot;
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        RelocationWorkloadHandlers.Accept(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            actor.Context.ObjectGeneration,
            message,
            evidence,
            context.Metadata.Find(
                RelocationWorkloadMetadata.OperationId));
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotActorRequestHandler(nameof(RelocationWorkloadRequest))]
internal sealed class PayloadUserSpotActorWorkloadRequestHandler(
    EvidenceStore evidence)
    : IZLinkSpotActorRequestHandler<
        RelocationPayloadUserSpot,
        TransferActor,
        RelocationWorkloadRequest,
        RelocationWorkloadReply>
{
    public ValueTask<RelocationWorkloadReply> HandleAsync(
        RelocationPayloadUserSpot spot,
        TransferActor actor,
        IZLinkMessageContext context,
        RelocationWorkloadRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(RelocationWorkloadHandlers.Reply(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            actor.Context.ObjectGeneration,
            request,
            evidence,
            context.Metadata.Find(
                RelocationWorkloadMetadata.OperationId),
            context.CorrelationId));
    }
}

[ZLinkSpotActorSendHandler(nameof(RelocationWorkloadPacket))]
internal sealed class PayloadUserSpotActorWorkloadSendHandler(
    EvidenceStore evidence)
    : IZLinkSpotActorSendHandler<
        RelocationPayloadUserSpot,
        TransferActor,
        RelocationWorkloadPacket>
{
    public ValueTask HandleAsync(
        RelocationPayloadUserSpot spot,
        TransferActor actor,
        IZLinkMessageContext context,
        RelocationWorkloadPacket message,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        RelocationWorkloadHandlers.Accept(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            actor.Context.ObjectGeneration,
            message,
            evidence,
            context.Metadata.Find(
                RelocationWorkloadMetadata.OperationId));
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotActorRequestHandler(nameof(RelocationWorkloadRequest))]
internal sealed class PerActorUserSpotActorWorkloadRequestHandler(
    EvidenceStore evidence)
    : IZLinkSpotActorRequestHandler<
        RelocationPayloadPerActorUserSpot,
        TransferActor,
        RelocationWorkloadRequest,
        RelocationWorkloadReply>
{
    public ValueTask<RelocationWorkloadReply> HandleAsync(
        RelocationPayloadPerActorUserSpot spot,
        TransferActor actor,
        IZLinkMessageContext context,
        RelocationWorkloadRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(RelocationWorkloadHandlers.Reply(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            actor.Context.ObjectGeneration,
            request,
            evidence,
            context.Metadata.Find(
                RelocationWorkloadMetadata.OperationId),
            context.CorrelationId));
    }
}

[ZLinkSpotActorSendHandler(nameof(RelocationWorkloadPacket))]
internal sealed class PerActorUserSpotActorWorkloadSendHandler(
    EvidenceStore evidence)
    : IZLinkSpotActorSendHandler<
        RelocationPayloadPerActorUserSpot,
        TransferActor,
        RelocationWorkloadPacket>
{
    public ValueTask HandleAsync(
        RelocationPayloadPerActorUserSpot spot,
        TransferActor actor,
        IZLinkMessageContext context,
        RelocationWorkloadPacket message,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        RelocationWorkloadHandlers.Accept(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            actor.Context.ObjectGeneration,
            message,
            evidence,
            context.Metadata.Find(
                RelocationWorkloadMetadata.OperationId));
        return ValueTask.CompletedTask;
    }
}

internal abstract class SpotWorkloadHandler
{
    protected SpotWorkloadHandler(EvidenceStore evidence) => Evidence = evidence;

    protected EvidenceStore Evidence { get; }
}

internal sealed class PayloadUserSpotWorkloadRequestHandler(EvidenceStore evidence)
    : SpotWorkloadHandler(evidence),
        IZLinkSpotRequestHandler<
            RelocationPayloadUserSpot,
            RelocationWorkloadRequest,
            RelocationWorkloadReply>
{
    public ValueTask<RelocationWorkloadReply> HandleAsync(
        RelocationPayloadUserSpot spot,
        RelocationWorkloadRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(RelocationWorkloadHandlers.Reply(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString(),
            spot.Context.ObjectGeneration,
            request,
            Evidence,
            observedOperationId: null,
            correlationId: null));
    }
}

internal sealed class PayloadUserSpotRelocationQueueBlockHandler(
    EvidenceStore evidence,
    TransferGateStore gates)
    : IZLinkSpotRequestHandler<
        RelocationPayloadUserSpot,
        RelocationQueueBlockReq,
        RelocationQueueBlockRes>
{
    public async ValueTask<RelocationQueueBlockRes> HandleAsync(
        RelocationPayloadUserSpot spot,
        RelocationQueueBlockReq request,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            request.Scenario,
            spot.Context.SpotId,
            "queue_block_started",
            spot.Context.NodeRid.ToString());
        await gates.WaitAsync(request.TargetId, cancellationToken)
            .ConfigureAwait(false);
        evidence.Add(
            request.Scenario,
            spot.Context.SpotId,
            "queue_block_released",
            spot.Context.NodeRid.ToString());
        return new RelocationQueueBlockRes(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString());
    }
}

internal sealed class PayloadUserSpotWorkloadSendHandler(EvidenceStore evidence)
    : SpotWorkloadHandler(evidence),
        IZLinkSpotPacketHandler<
            RelocationPayloadUserSpot,
            RelocationWorkloadPacket>
{
    public ValueTask HandleAsync(
        RelocationPayloadUserSpot spot,
        RelocationWorkloadPacket message,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        RelocationWorkloadHandlers.Accept(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString(),
            spot.Context.ObjectGeneration,
            message,
            Evidence,
            observedOperationId: null);
        return ValueTask.CompletedTask;
    }
}

internal sealed class PayloadPerActorUserSpotWorkloadRequestHandler(
    EvidenceStore evidence)
    : SpotWorkloadHandler(evidence),
        IZLinkSpotRequestHandler<
            RelocationPayloadPerActorUserSpot,
            RelocationWorkloadRequest,
            RelocationWorkloadReply>
{
    public ValueTask<RelocationWorkloadReply> HandleAsync(
        RelocationPayloadPerActorUserSpot spot,
        RelocationWorkloadRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(RelocationWorkloadHandlers.Reply(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString(),
            spot.Context.ObjectGeneration,
            request,
            Evidence,
            observedOperationId: null,
            correlationId: null));
    }
}

internal sealed class PayloadPerActorUserSpotWorkloadSendHandler(
    EvidenceStore evidence)
    : SpotWorkloadHandler(evidence),
        IZLinkSpotPacketHandler<
            RelocationPayloadPerActorUserSpot,
            RelocationWorkloadPacket>
{
    public ValueTask HandleAsync(
        RelocationPayloadPerActorUserSpot spot,
        RelocationWorkloadPacket message,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        RelocationWorkloadHandlers.Accept(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString(),
            spot.Context.ObjectGeneration,
            message,
            Evidence,
            observedOperationId: null);
        return ValueTask.CompletedTask;
    }
}

internal sealed class PayloadInstanceSpotWorkloadRequestHandler(EvidenceStore evidence)
    : SpotWorkloadHandler(evidence),
        IZLinkSpotRequestHandler<
            RelocationPayloadInstanceSpot,
            RelocationWorkloadRequest,
            RelocationWorkloadReply>
{
    public ValueTask<RelocationWorkloadReply> HandleAsync(
        RelocationPayloadInstanceSpot spot,
        RelocationWorkloadRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(RelocationWorkloadHandlers.Reply(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString(),
            spot.Context.ObjectGeneration,
            request,
            Evidence,
            observedOperationId: null,
            correlationId: null));
    }
}

internal sealed class PayloadInstanceSpotWorkloadSendHandler(EvidenceStore evidence)
    : SpotWorkloadHandler(evidence),
        IZLinkSpotPacketHandler<
            RelocationPayloadInstanceSpot,
            RelocationWorkloadPacket>
{
    public ValueTask HandleAsync(
        RelocationPayloadInstanceSpot spot,
        RelocationWorkloadPacket message,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        RelocationWorkloadHandlers.Accept(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString(),
            spot.Context.ObjectGeneration,
            message,
            Evidence,
            observedOperationId: null);
        return ValueTask.CompletedTask;
    }
}

internal sealed class RelocationReadyUserSpotWorkloadRequestHandler(
    EvidenceStore evidence)
    : SpotWorkloadHandler(evidence),
        IZLinkSpotRequestHandler<
            RelocationReadyUserSpot,
            RelocationWorkloadRequest,
            RelocationWorkloadReply>
{
    public ValueTask<RelocationWorkloadReply> HandleAsync(
        RelocationReadyUserSpot spot,
        RelocationWorkloadRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(RelocationWorkloadHandlers.Reply(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString(),
            spot.Context.ObjectGeneration,
            request,
            Evidence,
            observedOperationId: null,
            correlationId: null));
    }
}

internal sealed class RelocationReadyDefaultUserSpotWorkloadRequestHandler(
    EvidenceStore evidence)
    : SpotWorkloadHandler(evidence),
        IZLinkSpotRequestHandler<
            RelocationReadyDefaultUserSpot,
            RelocationWorkloadRequest,
            RelocationWorkloadReply>
{
    public ValueTask<RelocationWorkloadReply> HandleAsync(
        RelocationReadyDefaultUserSpot spot,
        RelocationWorkloadRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(RelocationWorkloadHandlers.Reply(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString(),
            spot.Context.ObjectGeneration,
            request,
            Evidence,
            observedOperationId: null,
            correlationId: null));
    }
}
