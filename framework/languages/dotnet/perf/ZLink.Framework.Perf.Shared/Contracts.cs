using System.Text.Json;
using System.Text.Json.Serialization;

namespace ZLink.Framework.Perf;

// Perf spec §12/§15.2. Decimal strings are application fields, not a message codec.
public record Identity
{
    public required string runId { get; init; }
    public required string cellId { get; init; }
    public required string resetSeq { get; init; }
    public required string phase { get; init; }
}

public sealed record PerfEchoRequest : Identity
{
    public required int clientId { get; init; }
    public required string sequence { get; init; }
    public required string correlationId { get; init; }
    public required string sentTicks { get; init; }
    public required string clockDomainId { get; init; }
    public required string? returnSpotId { get; init; }
    public required string? returnChannel { get; init; }
    public required string payload { get; init; }
}

public sealed record PerfEchoReply : Identity
{
    public required int clientId { get; init; }
    public required string sequence { get; init; }
    public required string correlationId { get; init; }
    public required string receivedTicks { get; init; }
    public required string clockDomainId { get; init; }
    public required string payload { get; init; }
}

public sealed record PerfDriveRequest(PerfEchoRequest echo);
public sealed record PerfDriveReply(bool started, PerfEchoReply? echo);
public sealed record PerfTriggerRequest : Identity;
public sealed record PerfTriggerReply : Identity
{
    public required bool accepted { get; init; }
    public required string state { get; init; }
    public required string configHash { get; init; }
    public required string? reason { get; init; }
}
public sealed record PerfPublishEvent : Identity
{
    public required string sequence { get; init; }
    public required string topic { get; init; }
    public required string sentTicks { get; init; }
    public required string clockDomainId { get; init; }
    public required string payload { get; init; }
}
public sealed record WorkerObservation(string startedTicks, string endedTicks, string clockDomainId,
    string iterations, uint checksum);
public sealed record ResetRequest
{
    public required string runId { get; init; }
    public required string cellId { get; init; }
    public required string resetSeq { get; init; }
}
public sealed record ResetReply(bool ok, string runId, string cellId, string role, int roleInstance,
    string resetSeq, string applicationResetAtUnixMs, string? capacityEpoch, string? reason,
    Dictionary<string, NullReason> nullReasons);
public sealed record PerfReady(string runId, string cellId, string role, int roleInstance,
    bool infrastructureReady, bool objectsReady, bool consumersReady, bool ready,
    string observedAtUnixMs, object[] evidence, string[] reasons);
public sealed record NullReason(string code, string reason, string owner = "perf/README.ko.md",
    double? lowerBoundMs = null);
public sealed record Window(string? startedAtUnixMs, string? endedAtUnixMs, string? startTicks,
    string? endTicks, double? measuredSeconds, double? settleSeconds);
public sealed record ClockMetadata(string source, string nativeFrequencyHz, string ticksUnit,
    string clockDomainId, string scope, string? alignmentMethod, string? maxErrorNs,
    string? validFromTicks, string? validThroughTicks, string[] evidence);
public sealed record SerializedMessageBytes(string direction, string packetName,
    string logicalPayloadBytes, string? observedSerializedBytes);
public sealed record PerfMetricsSnapshot(int schemaVersion, string runId, string cellId, string resetSeq,
    string language, string role, int roleInstance, string configHash, string phase, Window window,
    ClockMetadata clock, SerializedMessageBytes[] serializedMessageBytes,
    Dictionary<string, object?> metrics, Dictionary<string, object?> histograms,
    Dictionary<string, NullReason> nullReasons, object? publicStatus, object[] publicMetrics,
    Dictionary<string, object?> runtimeMetrics, Dictionary<string, object?> provenance);

public sealed record Workload(int payloadSize, double durationSeconds, double warmupSeconds,
    int inflight, int? connections, int? logicalStreams, int clientCount, int? connectConcurrency,
    int requestTimeoutMs, int correlationExpiryMs, int settleTimeoutMs, int setupTimeoutMs,
    int adminTimeoutMs, int socketSendTimeoutMs);
public sealed record RoleConfig(string runId, string cellId, string configHash, string role,
    int roleInstance, string scenario, string? topology, string? channelName, string? meshName,
    string? listenerEndpoint, string? peerEndpoint, string metricsUrl, string applicationTriggerUrl,
    bool source, string objectRole, object? store, string[] spotIds, string[] actorIds,
    string executionMode, Workload workload, Dictionary<string, object?> provenance, DiagnosticsConfig? diagnostics = null);
public sealed record DiagnosticsConfig(string level, string flowFile);
public sealed record EndpointRole(string role, int roleInstance, string configFile,
    string? streamEndpoint, string applicationTriggerUrl, MetricsEndpoint metrics,
    Dictionary<string, string> transportEndpoints, string[] spotIds, string[] actorIds);
public sealed record MetricsEndpoint(string transport, string baseUrl);
public sealed record EndpointManifest(string runId, string cellId, string configHash,
    Workload workload, EndpointRole[] roles, Dictionary<string, object?> provenance);

public static class PerfJson
{
    // This serializer is only for application admin/config/result files. Framework messages use
    // the packages' default typed JSON serializers without registering any codec.
    public static JsonSerializerOptions Options { get; } = Create();
    private static JsonSerializerOptions Create()
    {
        var options = new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            PropertyNameCaseInsensitive = false,
            UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow
        };
        options.Converters.Add(new U64JsonConverter());
        options.Converters.Add(new I64JsonConverter());
        options.Converters.Add(new JsonStringEnumConverter());
        return options;
    }
    public static T Read<T>(string text) => JsonSerializer.Deserialize<T>(text, Options)
        ?? throw new JsonException("JSON null is not a document.");
    public static string Write<T>(T value) => JsonSerializer.Serialize(value, Options);
}

public sealed class U64JsonConverter : JsonConverter<ulong>
{
    public override ulong Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options) =>
        reader.TokenType == JsonTokenType.String ? DecimalText.U64(reader.GetString()!) :
        throw new JsonException("U64 requires a decimal string.");
    public override void Write(Utf8JsonWriter writer, ulong value, JsonSerializerOptions options) =>
        writer.WriteStringValue(DecimalText.Of(value));
}
public sealed class I64JsonConverter : JsonConverter<long>
{
    public override long Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options) =>
        reader.TokenType == JsonTokenType.String ? DecimalText.I64(reader.GetString()!) :
        throw new JsonException("I64 requires a decimal string.");
    public override void Write(Utf8JsonWriter writer, long value, JsonSerializerOptions options) =>
        writer.WriteStringValue(DecimalText.Of(value));
}
