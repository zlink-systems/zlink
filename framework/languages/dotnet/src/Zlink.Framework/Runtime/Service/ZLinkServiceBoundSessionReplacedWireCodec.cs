using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

internal static partial class ZLinkServiceWireCodec
{
    /// <summary>
    /// The Actor authority fence carried by the one-way bound-session
    /// replacement notification. It authenticates the transport source; the
    /// receiver never uses it to locate its local Actor binding.
    /// </summary>
    internal readonly record struct BoundSessionReplacedActorAuthority(
        string ActorId,
        ulong ObjectGeneration,
        RoutingId TargetNodeRid,
        ulong TargetNodeGeneration,
        ulong ExpectedAuthorityOwnerGeneration,
        ulong ExpectedOwnerLeaseGeneration);

    /// <summary>
    /// The exact retired session owner identity. Every field participates in
    /// the receiver lifecycle fence.
    /// </summary>
    internal readonly record struct BoundSessionReplacedRetiredSession(
        RoutingId SessionOwnerNodeRid,
        ulong SessionOwnerNodeGeneration,
        string SessionOwnerId,
        ulong SessionOwnerLeaseGeneration,
        RoutingId SessionRid,
        ulong RetiredBindingGeneration);

    internal readonly record struct BoundSessionReplacedRecord(
        BoundSessionReplacedActorAuthority ActorAuthority,
        BoundSessionReplacedRetiredSession RetiredSession);

    internal static byte[] EncodeBoundSessionReplaced(
        BoundSessionReplacedRecord record)
    {
        ValidateBoundSessionReplaced(record);
        var writer = new WireWriter();
        writer.Text8(record.ActorAuthority.ActorId);
        writer.U64(record.ActorAuthority.ObjectGeneration);
        writer.Rid(record.ActorAuthority.TargetNodeRid);
        writer.U64(record.ActorAuthority.TargetNodeGeneration);
        writer.U64(record.ActorAuthority.ExpectedAuthorityOwnerGeneration);
        writer.U64(record.ActorAuthority.ExpectedOwnerLeaseGeneration);
        writer.Rid(record.RetiredSession.SessionOwnerNodeRid);
        writer.U64(record.RetiredSession.SessionOwnerNodeGeneration);
        writer.Text8(record.RetiredSession.SessionOwnerId);
        writer.U64(record.RetiredSession.SessionOwnerLeaseGeneration);
        writer.Rid(record.RetiredSession.SessionRid);
        writer.U64(record.RetiredSession.RetiredBindingGeneration);

        var bytes = Prefix(
            ServiceWireConstants.Command.BoundSessionReplaced,
            ServiceWireConstants.Flag.None,
            writer.Count);
        writer.CopyTo(bytes.AsSpan(5));
        return bytes;
    }

    internal static bool TryDecodeBoundSessionReplaced(
        ReadOnlySpan<byte> bytes,
        out BoundSessionReplacedRecord record,
        out DecodeError error)
    {
        record = default;
        try
        {
            if (!TryDecodePrefix(bytes, out var command, out var flags, out error))
                return false;
            if (command != ServiceWireConstants.Command.BoundSessionReplaced)
            {
                error = DecodeError.UnknownCommand;
                return false;
            }
            if (flags != ServiceWireConstants.Flag.None)
            {
                error = DecodeError.ForbiddenFlag;
                return false;
            }

            var reader = new WireReader(bytes[5..]);
            if (!reader.TryText8(out var actorId)
                || !reader.TryU64(out var objectGeneration)
                || !reader.TryRid(out var targetNodeRid)
                || !reader.TryU64(out var targetNodeGeneration)
                || !reader.TryU64(out var authorityOwnerGeneration)
                || !reader.TryU64(out var ownerLeaseGeneration)
                || !reader.TryRid(out var sessionOwnerNodeRid)
                || !reader.TryU64(out var sessionOwnerNodeGeneration)
                || !reader.TryText8(out var sessionOwnerId)
                || !reader.TryU64(out var sessionOwnerLeaseGeneration)
                || !reader.TryRid(out var sessionRid)
                || !reader.TryU64(out var retiredBindingGeneration)
                || reader.Remaining != 0)
                return DecodeBoundSessionReplacedFailure(
                    ref reader,
                    out error);

            var decoded = new BoundSessionReplacedRecord(
                new BoundSessionReplacedActorAuthority(
                    actorId,
                    objectGeneration,
                    targetNodeRid,
                    targetNodeGeneration,
                    authorityOwnerGeneration,
                    ownerLeaseGeneration),
                new BoundSessionReplacedRetiredSession(
                    sessionOwnerNodeRid,
                    sessionOwnerNodeGeneration,
                    sessionOwnerId,
                    sessionOwnerLeaseGeneration,
                    sessionRid,
                    retiredBindingGeneration));
            ValidateBoundSessionReplaced(decoded);
            record = decoded;
            error = DecodeError.None;
            return true;
        }
        catch (Exception exception)
            when (exception is ArgumentException
                or FormatException
                or OverflowException)
        {
            record = default;
            error = DecodeError.InvalidField;
            return false;
        }
    }

    private static bool DecodeBoundSessionReplacedFailure(
        ref WireReader reader,
        out DecodeError error)
    {
        error = reader.Truncated
            ? DecodeError.TruncatedField
            : reader.Remaining == 0
                ? DecodeError.InvalidField
                : DecodeError.TrailingByte;
        return false;
    }

    private static void ValidateBoundSessionReplaced(
        BoundSessionReplacedRecord record)
    {
        var actor = record.ActorAuthority;
        var retired = record.RetiredSession;
        if (string.IsNullOrWhiteSpace(actor.ActorId)
            || actor.ActorId.Contains('\0')
            || actor.ObjectGeneration == 0
            || actor.TargetNodeRid.IsEmpty
            || actor.TargetNodeGeneration == 0
            || actor.ExpectedAuthorityOwnerGeneration == 0
            || actor.ExpectedOwnerLeaseGeneration == 0
            || retired.SessionOwnerNodeRid.IsEmpty
            || retired.SessionOwnerNodeGeneration == 0
            || string.IsNullOrWhiteSpace(retired.SessionOwnerId)
            || retired.SessionOwnerId.Contains('\0')
            || retired.SessionOwnerLeaseGeneration == 0
            || retired.SessionRid.IsEmpty
            || retired.RetiredBindingGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(record));
    }
}
