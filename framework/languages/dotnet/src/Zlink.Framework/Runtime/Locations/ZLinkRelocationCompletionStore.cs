namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkRelocationCompletionStore(IZLinkLocationRepository authorities)
{
    internal ValueTask<ZLinkRelocationReplyRow?> PublishAsync(
        ZLinkRelocationReplyRow result,
        Func<string> failureReply,
        CancellationToken cancellationToken
    ) =>
        authorities.UpdateRelocationReplyAsync(
            result,
            current =>
            {
                if (current.Entry.Delivery != ZLinkRelocationCompletionDelivery.Accepted)
                    return current;
                var pending = current with
                {
                    Entry = current.Entry with
                    {
                        Delivery = ZLinkRelocationCompletionDelivery.Pending,
                        Reply = result.Entry.Reply,
                    },
                };
                var terminal = pending with
                {
                    Entry = pending.Entry with
                    {
                        Delivery = ZLinkRelocationCompletionDelivery.SourceLeaseExpired,
                    },
                };
                return terminal.Encode().Length <= ZLinkRelocationReplyRow.MaximumBytes
                    ? pending
                    : pending with
                    {
                        Entry = pending.Entry with { Reply = failureReply() },
                    };
            },
            cancellationToken
        );

    internal ValueTask<ZLinkRelocationReplyRow?> AcknowledgeAsync(
        ZLinkRelocationReplyRow row,
        ZLinkRelocationCompletionDelivery evidence,
        CancellationToken cancellationToken
    )
    {
        if (
            evidence
            is ZLinkRelocationCompletionDelivery.Accepted
                or ZLinkRelocationCompletionDelivery.Pending
        )
            throw new ArgumentOutOfRangeException(nameof(evidence));
        return authorities.UpdateRelocationReplyAsync(
            row,
            current =>
                current.Entry.Delivery == ZLinkRelocationCompletionDelivery.Pending
                    ? current with
                    {
                        Entry = current.Entry with { Delivery = evidence },
                    }
                    : current,
            cancellationToken
        );
    }

    internal ValueTask<ZLinkRelocationReplyRow?> RenewAsync(
        ZLinkRelocationReplyRow row,
        CancellationToken cancellationToken
    ) => authorities.UpdateRelocationReplyAsync(row, static current => current, cancellationToken);
}
