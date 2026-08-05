namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkRelocationReplyLifetime
{
    internal static readonly TimeSpan TerminalRetention = TimeSpan.FromHours(24);
}

internal sealed class ZLinkRelocationReplyTarget(
    IZLinkBackendRelocationReplyRelay backend) : IRelocationReplyRelayTarget
{
    public ValueTask<ZLinkServiceWireCodec.ReplyRelayAckRecord?> RelayAsync(
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        IReadOnlyList<Message> payload,
        CancellationToken cancellationToken)
    {
        var consumed = false;
        try
        {
            if (relay.Coordinator.NodeRid != sourceNodeRid
                || relay.Coordinator.NodeGeneration != sourceNodeGeneration)
                return ValueTask.FromResult<
                    ZLinkServiceWireCodec.ReplyRelayAckRecord?>(null);
            cancellationToken.ThrowIfCancellationRequested();
            var completion = backend.TryCompleteRelocationReply(relay, payload);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_reply_relay_completion operation={relay.OperationId.High:x16}{relay.OperationId.Low:x16} "
                + $"reply_route={relay.ReplyRouteId:x16} state={completion.State}");
            if (completion.State == ZLinkRelocationReplyCompletionState.NotFound)
                return ValueTask.FromResult<
                    ZLinkServiceWireCodec.ReplyRelayAckRecord?>(null);
            consumed = true;
            return ValueTask.FromResult<
                ZLinkServiceWireCodec.ReplyRelayAckRecord?>(
                new ZLinkServiceWireCodec.ReplyRelayAckRecord(
                    relay.RelocationId,
                    relay.Coordinator,
                    relay.OperationId,
                    relay.ReplyRouteId,
                    completion.RequestSource,
                    (byte)completion.State));
        }
        finally
        {
            if (!consumed)
                ZLinkMessageParts.DisposeAll(payload);
        }
    }
}
