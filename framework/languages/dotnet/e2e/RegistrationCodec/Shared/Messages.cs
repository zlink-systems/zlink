using MessagePack;
using Zlink.Framework.Contracts.Handlers;

namespace RegistrationCodec.Shared;

public static class RegistrationCodecNames
{
    public const string Channel = "reg-codec";
}

[ZLinkPacket("EchoAttr")]
public sealed record EchoAttrReq(string Value);

public sealed record EchoRes(string Value, string ContentType);

[ZLinkPacket("EchoAttrMsg")]
public sealed record EchoAttrMsg(string CommandId, string Value);

[ZLinkPacket("EchoDi")]
public sealed record EchoDiReq(string Value);

[ZLinkPacket("EchoAuto")]
public sealed record EchoAutoReq(string Value);

[ZLinkPacket("EchoAutoMsg")]
public sealed record EchoAutoMsg(string CommandId, string Value);

[ZLinkPacket("EchoManual")]
public sealed record EchoManualReq(string Value);

[ZLinkPacket("EchoManualMsg")]
public sealed record EchoManualMsg(string CommandId, string Value);

[ZLinkPacket("EchoJson")]
public sealed record JsonEchoReq(string Value);

[ZLinkPacket("EchoJsonMsg")]
public sealed record JsonEchoMsg(string CommandId, string Value);

public sealed record CodecMismatchProbeRes(
    bool Rejected,
    string? FailureType,
    string? Value);

[ZLinkPacket("JsonGolden")]
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
[ZLinkPacket("EchoMessagePack")]
public sealed class PackedEchoReq
{
    [Key(0)] public string Value { get; set; } = string.Empty;
}

[MessagePackObject]
[ZLinkPacket("EchoMessagePackMsg")]
public sealed class PackedEchoMsg
{
    [Key(0)] public string CommandId { get; set; } = string.Empty;

    [Key(1)] public string Value { get; set; } = string.Empty;
}
