namespace Zlink.Framework.Runtime.Locations;

internal enum ZLinkRelocationCompletionDelivery
{
    Pending,
    TerminalReceived,
    AlreadyTerminal,
    SourceLeaseExpired,
    Accepted,
}

internal sealed record ZLinkRelocationCompletionEntry(
    string ObjectKey,
    string OperationId,
    ulong ReplyRouteId,
    string Reply,
    ZLinkCanonicalRequestSourceFence Source,
    ZLinkRelocationCompletionDelivery Delivery = ZLinkRelocationCompletionDelivery.Pending
);
