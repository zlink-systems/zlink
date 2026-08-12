using MessagePack;
using Zlink.Framework.Contracts.Handlers;

namespace RegistrationCodec.Shared;

public static class RegistrationCodecNames
{
    public const string Channel = "reg-codec";
}

[ZLinkPacket("EchoAttrReq")]
public sealed record EchoAttrReq(string Value);

public sealed record EchoRes(string Value, string ContentType);

[ZLinkPacket("EchoAttrMsg")]
public sealed record EchoAttrMsg(string CommandId, string Value);

[ZLinkPacket("EchoDiReq")]
public sealed record EchoDiReq(string Value);

[ZLinkPacket("EchoAutoReq")]
public sealed record EchoAutoReq(string Value);

[ZLinkPacket("EchoAutoMsg")]
public sealed record EchoAutoMsg(string CommandId, string Value);

[ZLinkPacket("EchoManualReq")]
public sealed record EchoManualReq(string Value);

[ZLinkPacket("EchoManualMsg")]
public sealed record EchoManualMsg(string CommandId, string Value);

[ZLinkPacket("JsonEchoReq")]
public sealed record JsonEchoReq(string Value);

[ZLinkPacket("JsonEchoMsg")]
public sealed record JsonEchoMsg(string CommandId, string Value);

public sealed record CodecMismatchProbeRes(
    bool Rejected,
    string? FailureType,
    string? Value);

[ZLinkPacket("JsonGoldenReq")]
public sealed record JsonGoldenReq(
    string DisplayName,
    string Status,
    long Balance,
    byte[] Payload,
    int Score,
    double Ratio,
    string? OptionalNote);

public sealed record JsonGoldenRes(
    string DisplayName,
    string Status,
    long Balance,
    byte[] Payload,
    int Score,
    double Ratio,
    string? OptionalNote,
    string ContentType);

public sealed record EvidenceWaitReq(
    string[] ContainsAll,
    int TimeoutMilliseconds = 10000);

[MessagePackObject]
[ZLinkPacket("PackedEchoReq")]
public sealed class PackedEchoReq
{
    [Key(0)] public string Value { get; set; } = string.Empty;
}

[MessagePackObject]
[ZLinkPacket("PackedEchoRes")]
public sealed class PackedEchoRes
{
    [Key(0)] public string Value { get; set; } = string.Empty;
}

[MessagePackObject]
[ZLinkPacket("PackedEchoMsg")]
public sealed class PackedEchoMsg
{
    [Key(0)] public string CommandId { get; set; } = string.Empty;

    [Key(1)] public string Value { get; set; } = string.Empty;
}
