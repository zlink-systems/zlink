using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Systems.Zlink;

/// <summary>
/// Identifies one exact Actor incarnation and the owner route observed with it.
/// </summary>
[JsonConverter(typeof(ActorRefJsonConverter))]
public readonly record struct ActorRef(
    string ActorId,
    ulong ObjectGeneration,
    string MeshName,
    RoutingId NodeRid)
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private readonly string _actorId = ValidateActorId(ActorId);
    private readonly ulong _objectGeneration = ValidateObjectGeneration(ObjectGeneration);
    private readonly string _meshName = ValidateMeshName(MeshName);
    private readonly RoutingId _nodeRid = ValidateNodeRid(NodeRid);

    public string ActorId
    {
        get => _actorId;
        init => _actorId = ValidateActorId(value);
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

    private static string ValidateActorId(string? value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value);
        if (value!.Contains('\0'))
            throw new ArgumentOutOfRangeException(
                nameof(value),
                "Actor ID must not contain NUL.");

        int byteCount;
        try
        {
            byteCount = StrictUtf8.GetByteCount(value);
        }
        catch (EncoderFallbackException exception)
        {
            throw new ArgumentException(
                "Actor ID must contain valid UTF-8 text.",
                nameof(value),
                exception);
        }

        if (byteCount > byte.MaxValue)
            throw new ArgumentOutOfRangeException(
                nameof(value),
                "Actor ID must be 1..255 UTF-8 bytes.");
        return value;
    }

    private static ulong ValidateObjectGeneration(ulong value)
    {
        if (value is 0 or > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(value));
        return value;
    }

    private static string ValidateMeshName(string value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value);
        return value;
    }

    private static RoutingId ValidateNodeRid(RoutingId value)
    {
        if (value.IsEmpty)
            throw new ArgumentException(
                "Actor owner routing id must not be empty.",
                nameof(value));
        return value;
    }

    private sealed class ActorRefJsonConverter : JsonConverter<ActorRef>
    {
        public override bool HandleNull => true;

        public override ActorRef Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options)
        {
            if (reader.TokenType != JsonTokenType.StartObject)
                throw new JsonException("ActorRef must be a JSON object.");

            string? actorId = null;
            string? objectGeneration = null;
            string? meshName = null;
            string? nodeRid = null;
            var seen = new HashSet<string>(StringComparer.Ordinal);
            while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
            {
                if (reader.TokenType != JsonTokenType.PropertyName)
                    throw new JsonException("ActorRef property name is required.");

                var property = reader.GetString()
                               ?? throw new JsonException("ActorRef property name is required.");
                if (!seen.Add(property))
                    throw new JsonException($"Duplicate ActorRef property '{property}'.");
                if (!reader.Read())
                    throw new JsonException("ActorRef property value is missing.");

                switch (property)
                {
                    case "actorId":
                        actorId = ReadString(ref reader, property);
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
                        throw new JsonException($"Unknown ActorRef property '{property}'.");
                }
            }

            if (reader.TokenType != JsonTokenType.EndObject
                || actorId is null
                || objectGeneration is null
                || meshName is null
                || nodeRid is null)
                throw new JsonException(
                    "ActorRef requires actorId, objectGeneration, meshName and nodeRid.");

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
                throw new JsonException("ActorRef objectGeneration must be a canonical decimal string.");

            try
            {
                return new ActorRef(
                    actorId,
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
                throw new JsonException("ActorRef contains an invalid value.", exception);
            }
        }

        public override void Write(
            Utf8JsonWriter writer,
            ActorRef value,
            JsonSerializerOptions options)
        {
            var actorId = ValidateActorId(value.ActorId);
            var objectGeneration = ValidateObjectGeneration(value.ObjectGeneration);
            var meshName = ValidateMeshName(value.MeshName);
            var nodeRid = ValidateNodeRid(value.NodeRid);

            writer.WriteStartObject();
            writer.WriteString("actorId", actorId);
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
                throw new JsonException($"ActorRef {property} must be a string.");
            return reader.GetString()
                   ?? throw new JsonException($"ActorRef {property} must not be null.");
        }
    }
}
