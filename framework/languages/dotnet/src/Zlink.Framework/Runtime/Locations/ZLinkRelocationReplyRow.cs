using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkRelocationReplyRow(
    Guid RelocationId,
    ZLinkLocationOwnerToken Writer,
    ZLinkRelocationCompletionEntry Entry
)
{
    internal const int MaximumBytes = 1024 * 1024;
    internal static readonly TimeSpan ActiveRetention = TimeSpan.FromHours(24);
    internal static readonly TimeSpan RenewThreshold = TimeSpan.FromHours(12);
    internal static readonly TimeSpan TerminalRetention = TimeSpan.FromMinutes(5);

    private static readonly JsonSerializerOptions Json = new(ZLinkCanonicalJson.Options)
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    internal static string Prefix(Guid relocationId) =>
        $"$zlink/relocation-reply/{relocationId:D}/";

    internal ZLinkStoreKey Key =>
        new(
            FormattableString.Invariant(
                $"{Prefix(RelocationId)}{Entry.Source.OwnerId}.{Entry.Source.OwnerLeaseGeneration}.{Entry.Source.NodeRid}.{Entry.Source.NodeGeneration}/{Entry.OperationId}"
            )
        );

    internal bool IsActive =>
        Entry.Delivery
            is ZLinkRelocationCompletionDelivery.Accepted
                or ZLinkRelocationCompletionDelivery.Pending;

    internal byte[] Encode() =>
        JsonSerializer.SerializeToUtf8Bytes(
            new Wire(
                "1",
                Entry.ObjectKey,
                Entry.OperationId,
                RelocationId.ToString("D"),
                Entry.Delivery == ZLinkRelocationCompletionDelivery.Accepted ? null : Entry.Reply,
                Entry.ReplyRouteId.ToString(CultureInfo.InvariantCulture),
                new(
                    Entry.Source.OwnerLeaseGeneration.ToString(CultureInfo.InvariantCulture),
                    Entry.Source.NodeGeneration.ToString(CultureInfo.InvariantCulture),
                    Entry.Source.NodeRid,
                    Entry.Source.OwnerId
                ),
                JsonNamingPolicy.CamelCase.ConvertName(Entry.Delivery.ToString()),
                new(Writer.LeaseGeneration.ToString(CultureInfo.InvariantCulture), Writer.OwnerId)
            ),
            Json
        );

    internal static ZLinkRelocationReplyRow Decode(ZLinkStoreKey key, ReadOnlyMemory<byte> bytes)
    {
        try
        {
            if (bytes.Length > MaximumBytes)
                throw InvalidRow();
            var wire = JsonSerializer.Deserialize<Wire>(bytes.Span, Json);
            if (
                wire is not { FormatVersion: "1", Source: { } source, Writer: { } writer }
                || wire.ObjectKey is not { Length: > 7 } objectKey
                || !objectKey.StartsWith("zla1:", StringComparison.Ordinal)
                || objectKey[5] is not ('a' or 'u' or 's')
                || objectKey[6] != ':'
                || wire.OperationId is not { Length: 32 } operationId
                || !IsLowerHex(operationId)
                || operationId == "00000000000000000000000000000000"
                || source.NodeRid is not { Length: > 0 and <= 510 } rid
                || rid.Length % 2 != 0
                || !IsLowerHex(rid)
                || string.IsNullOrEmpty(source.OwnerId)
                || string.IsNullOrEmpty(writer.OwnerId)
            )
                throw InvalidRow();

            var state = wire.State switch
            {
                "accepted" => ZLinkRelocationCompletionDelivery.Accepted,
                "pending" => ZLinkRelocationCompletionDelivery.Pending,
                "terminalReceived" => ZLinkRelocationCompletionDelivery.TerminalReceived,
                "alreadyTerminal" => ZLinkRelocationCompletionDelivery.AlreadyTerminal,
                "sourceLeaseExpired" => ZLinkRelocationCompletionDelivery.SourceLeaseExpired,
                _ => throw InvalidRow(),
            };
            if (
                state == ZLinkRelocationCompletionDelivery.Accepted
                    ? wire.Reply is not null
                    : wire.Reply is not { } reply
                        || Convert.ToBase64String(Convert.FromBase64String(reply)) != reply
            )
                throw InvalidRow();

            var row = new ZLinkRelocationReplyRow(
                Guid.ParseExact(wire.RelocationId!, "D"),
                new(writer.OwnerId, checked((long)NonZero(writer.LeaseGeneration))),
                new(
                    objectKey,
                    operationId,
                    NonZero(wire.ReplyRouteId),
                    wire.Reply ?? string.Empty,
                    new(
                        source.OwnerId,
                        NonZero(source.LeaseGeneration),
                        rid,
                        NonZero(source.NodeGeneration)
                    ),
                    state
                )
            );
            if (
                row.RelocationId == Guid.Empty
                || row.Key != key
                || !bytes.Span.SequenceEqual(row.Encode())
            )
                throw InvalidRow();
            return row;
        }
        catch (Exception error)
            when (error
                    is JsonException
                        or ArgumentException
                        or FormatException
                        or OverflowException
            )
        {
            throw InvalidRow();
        }
    }

    private static ulong NonZero(string? value) =>
        ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var number)
        && number != 0
            ? number
            : throw InvalidRow();

    private static bool IsLowerHex(string value) =>
        value.All(character => character is (>= '0' and <= '9') or (>= 'a' and <= 'f'));

    private static ZLinkRelocationDataLostException InvalidRow() =>
        new("Relocation reply row is invalid.");

    private sealed record Wire(
        string? FormatVersion,
        string? ObjectKey,
        string? OperationId,
        string? RelocationId,
        string? Reply,
        string? ReplyRouteId,
        WireSource? Source,
        string? State,
        WireWriter? Writer
    );

    private sealed record WireSource(
        string? LeaseGeneration,
        string? NodeGeneration,
        string? NodeRid,
        string? OwnerId
    );

    private sealed record WireWriter(string? LeaseGeneration, string? OwnerId);
}
