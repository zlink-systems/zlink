using Systems.Zlink.Stream.Connector.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkAdmissionStreamPackets
{
    internal static ValueTask DeliverAsync(
        IZLinkStream stream,
        AdmissionDelivery delivery,
        ZlinkStreamHeader? request,
        CancellationToken cancellationToken
    )
    {
        var isTicket = delivery.Kind == "ticket";
        var payload = isTicket
            ? ZLinkFrameworkJsonPayloadCodec.Serialize(
                new TicketPayload(delivery.Ticket!, delivery.ExpiresInMs!.Value)
            )
            : ZLinkFrameworkJsonPayloadCodec.Serialize(
                new PositionPayload(delivery.Position!.Value, delivery.EstimatedWaitMs!.Value)
            );
        var header = request is null
            ? new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.None,
                null,
                isTicket
                    ? ZLinkAdmissionQueueNode.TicketKey
                    : ZlinkStreamHeaderCodec.AdmissionPositionName,
                ZlinkStreamMetadata.Empty
            )
            : ZLinkStreamReplyHeaders.CreateForRequest(
                request,
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.None,
                request.RequestSeq!.Value,
                ZlinkStreamMetadata.Empty
            );
        return ZLinkStreamFrameWriter.WriteAsync(stream, header, payload, cancellationToken);
    }

    private sealed record TicketPayload(string Ticket, int ExpiresInMs);

    private sealed record PositionPayload(int Position, int EstimatedWaitMs);
}
