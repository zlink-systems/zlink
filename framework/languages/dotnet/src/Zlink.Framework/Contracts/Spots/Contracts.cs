using System.Text;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Zlink.Framework.Contracts.Spots;

public enum ZLinkSpotCreateState
{
    Existing = 0,
    Created = 1,
    Rejected = 2
}

public readonly record struct ZLinkSpotCreateResponse(bool Accepted, ZLinkMessage? Reply)
{
    public static ZLinkSpotCreateResponse Accept(ZLinkMessage? reply = null)
    {
        var result = ZLinkSpotAcceptRejectResult.Accept(reply);
        return new ZLinkSpotCreateResponse(result.Accepted, result.Reply);
    }

    public static ZLinkSpotCreateResponse Accept<TReply>(TReply reply)
    {
        var result = ZLinkSpotAcceptRejectResult.Accept(reply);
        return new ZLinkSpotCreateResponse(result.Accepted, result.Reply);
    }

    public static ZLinkSpotCreateResponse Reject(ZLinkMessage? reply = null)
    {
        var result = ZLinkSpotAcceptRejectResult.Reject(reply);
        return new ZLinkSpotCreateResponse(result.Accepted, result.Reply);
    }

    public static ZLinkSpotCreateResponse Reject<TReply>(TReply reply)
    {
        var result = ZLinkSpotAcceptRejectResult.Reject(reply);
        return new ZLinkSpotCreateResponse(result.Accepted, result.Reply);
    }
}

[JsonConverter(typeof(SpotRefJsonConverter))]
public readonly record struct SpotRef(
    string SpotId,
    ulong ObjectGeneration,
    string MeshName,
    RoutingId NodeRid)
{
    private readonly string _spotId = ValidateSpotId(SpotId);
    private readonly ulong _objectGeneration = ValidateObjectGeneration(ObjectGeneration);
    private readonly string _meshName = ValidateMeshName(MeshName);
    private readonly RoutingId _nodeRid = ValidateNodeRid(NodeRid);

    public string SpotId
    {
        get => _spotId;
        init => _spotId = ValidateSpotId(value);
    }

    public ulong ObjectGeneration
    {
        get => _objectGeneration;
        init => _objectGeneration = ValidateObjectGeneration(value);
    }

    public string MeshName
    {
        get => _meshName;
        init => _meshName = ValidateMeshName(value);
    }

    public RoutingId NodeRid
    {
        get => _nodeRid;
        init => _nodeRid = ValidateNodeRid(value);
    }

    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    private static string ValidateSpotId(string? value)
    {
        if (string.IsNullOrEmpty(value) || value.Contains('\0'))
            throw new ArgumentException(
                "Spot ID must be valid UTF-8 with an encoded size of 1..255 bytes.",
                nameof(value));

        int byteCount;
        try
        {
            byteCount = StrictUtf8.GetByteCount(value);
        }
        catch (EncoderFallbackException exception)
        {
            throw new ArgumentException(
                "Spot ID must contain valid UTF-8 text.",
                nameof(value),
                exception);
        }

        if (byteCount is < 1 or > byte.MaxValue)
            throw new ArgumentOutOfRangeException(
                nameof(value),
                "Spot ID must be 1..255 UTF-8 bytes.");
        return value!;
    }

    private static ulong ValidateObjectGeneration(ulong value)
    {
        if (value is 0 or > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(value));
        return value;
    }

    private static string ValidateMeshName(string? value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value);
        return value;
    }

    private static RoutingId ValidateNodeRid(RoutingId value)
    {
        if (value.IsEmpty)
            throw new ArgumentException(
                "Spot owner routing id must not be empty.",
                nameof(value));
        return value;
    }

    private sealed class SpotRefJsonConverter : JsonConverter<SpotRef>
    {
        public override bool HandleNull => true;

        public override SpotRef Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options)
        {
            if (reader.TokenType != JsonTokenType.StartObject)
                throw new JsonException("SpotRef must be a JSON object.");

            string? spotId = null;
            string? objectGeneration = null;
            string? meshName = null;
            string? nodeRid = null;
            var seen = new HashSet<string>(StringComparer.Ordinal);
            while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
            {
                if (reader.TokenType != JsonTokenType.PropertyName)
                    throw new JsonException("SpotRef property name is required.");

                var property = reader.GetString()
                               ?? throw new JsonException("SpotRef property name is required.");
                if (!seen.Add(property))
                    throw new JsonException($"Duplicate SpotRef property '{property}'.");
                if (!reader.Read())
                    throw new JsonException("SpotRef property value is missing.");

                switch (property)
                {
                    case "spotId":
                        spotId = ReadString(ref reader, property);
                        break;
                    case "objectGeneration":
                        objectGeneration = ReadString(ref reader, property);
                        break;
                    case "meshName":
                        meshName = ReadString(ref reader, property);
                        break;
                    case "nodeRid":
                        nodeRid = ReadString(ref reader, property);
                        break;
                    default:
                        throw new JsonException($"Unknown SpotRef property '{property}'.");
                }
            }

            if (reader.TokenType != JsonTokenType.EndObject
                || spotId is null
                || objectGeneration is null
                || meshName is null
                || nodeRid is null)
                throw new JsonException(
                    "SpotRef requires spotId, objectGeneration, meshName and nodeRid.");

            if (!long.TryParse(
                    objectGeneration,
                    NumberStyles.None,
                    CultureInfo.InvariantCulture,
                    out var generation)
                || generation <= 0
                || !string.Equals(
                    objectGeneration,
                    generation.ToString(CultureInfo.InvariantCulture),
                    StringComparison.Ordinal))
                throw new JsonException("SpotRef objectGeneration must be a canonical decimal string.");

            try
            {
                return new SpotRef(
                    spotId,
                    checked((ulong)generation),
                    meshName,
                    RoutingId.FromHex(nodeRid));
            }
            catch (Exception exception) when (
                exception is ArgumentException
                or ArgumentOutOfRangeException
                or FormatException
                or OverflowException)
            {
                throw new JsonException("SpotRef contains an invalid value.", exception);
            }
        }

        public override void Write(
            Utf8JsonWriter writer,
            SpotRef value,
            JsonSerializerOptions options)
        {
            var spotId = ValidateSpotId(value.SpotId);
            var objectGeneration = ValidateObjectGeneration(value.ObjectGeneration);
            var meshName = ValidateMeshName(value.MeshName);
            var nodeRid = ValidateNodeRid(value.NodeRid);

            writer.WriteStartObject();
            writer.WriteString("spotId", spotId);
            writer.WriteString(
                "objectGeneration",
                objectGeneration.ToString(CultureInfo.InvariantCulture));
            writer.WriteString("meshName", meshName);
            writer.WriteString("nodeRid", nodeRid.ToHex());
            writer.WriteEndObject();
        }

        private static string ReadString(ref Utf8JsonReader reader, string property)
        {
            if (reader.TokenType != JsonTokenType.String)
                throw new JsonException($"SpotRef {property} must be a string.");
            return reader.GetString()
                   ?? throw new JsonException($"SpotRef {property} must not be null.");
        }
    }
}

public readonly record struct ZLinkSpotCreateResult(
    SpotRef Spot,
    ZLinkSpotCreateState State,
    ZLinkMessage? Reply);

public interface IZLinkSpotManager
{
    IZLinkSpotCreateCall Create(string spotType);
    IZLinkSpotGetOrCreateCall GetOrCreate(string spotId, string spotType);
    ValueTask<SpotRef?> FindAsync(string spotId,
        CancellationToken cancellationToken = default);
    ValueTask<bool> CloseAsync(
        SpotRef spot,
        CancellationToken cancellationToken = default);
}

public interface IZLinkSpotCreateCall
{
    IZLinkSpotCreateCall InMesh(string meshName);
    IZLinkSpotCreateCall Request(ZLinkMessage request);
    IZLinkSpotCreateCall Request<TRequest>(TRequest request);
    IZLinkSpotCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkSpotCreateResult> Async(
        CancellationToken cancellationToken = default);
    ValueTask<ZLinkSpotCreateResult> Yield(
        CancellationToken cancellationToken = default);
}

public interface IZLinkSpotGetOrCreateCall
{
    IZLinkSpotGetOrCreateCall InMesh(string meshName);
    IZLinkSpotGetOrCreateCall Request(ZLinkMessage request);
    IZLinkSpotGetOrCreateCall Request<TRequest>(TRequest request);
    IZLinkSpotGetOrCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkSpotCreateResult> Async(
        CancellationToken cancellationToken = default);
    ValueTask<ZLinkSpotCreateResult> Yield(
        CancellationToken cancellationToken = default);
}

public interface IZLinkSpotPublisherClient
{
    IZLinkPublishCall Publish<TEvent>(
        string channelName,
        string topic,
        TEvent message);
}

public interface IZLinkSpotPacketHandler<TSpot, in TMessage>
    where TSpot : class
{
    ValueTask HandleAsync(
        TSpot spot,
        TMessage message,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotRequestHandler<TSpot, in TRequest, TReply>
    where TSpot : class
{
    ValueTask<TReply> HandleAsync(
        TSpot spot,
        TRequest request,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotSubscriptionHandler<TSpot, in TEvent>
    where TSpot : class
{
    ValueTask HandleAsync(
        TSpot spot,
        TEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotTimerHandler<TSpot>
    where TSpot : class
{
    ValueTask HandleAsync(
        TSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken);
}
