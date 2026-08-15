# .NET Codec Extension Public Interface

[.NET exact interface table of contents](README.en.md) · [Common Message Contract](../../../04-message-model.en.md) ·
[Common Framework API](../../../06-framework-api.en.md#9-codec)

## 1. Scope

This document only fixes the C# API an application uses to register a
codec extension, and the SPI an external codec provider implements. The
default JSON behavior, packet name decision, the JSON format of a global
object reference, and payload ownership are owned by the common message
contract. The internal registry, codec selection cache, and dispatch
implementation aren't this document's contract.

An application using only JSON doesn't call the codec API. An
application using an optional codec package registers one extension
instance with `IZLinkCodecRegistryBuilder.Use(...)`.
`IZLinkCodecRegistrar`, `IZLinkMessageSerializer`, and
`ZLinkEncodedPayload` are the SPI only a custom codec provider
implements.

## 2. Codec Registration API And Provider SPI

`IZLinkCodecRegistrar` only registers a business payload serializer. The
STREAM header's codec value is declared with the Stream Connector-owned
`IZlinkStreamCodecRegistration`. This separation keeps the HTTP client
from depending on the STREAM runtime or the compression package.

`contentType` takes a parameter-free ASCII `type/subtype`. At startup, the registry removes
leading and trailing SP and TAB and converts ASCII uppercase letters to lowercase. The
result is the canonical content type used as the registry key. A parameter, whitespace
inside the value, or a non-ASCII token causes `ArgumentException`. If the same canonical
content type is registered again, the last serializer replaces the earlier one.

A value received from the framework service wire must already be canonical. Another
representation completes with `ZLinkFrameworkErrorKind.ProtocolError`. The HTTP client
first parses response parameters and passes only the parameter-free media type to this
rule.

Send-side serializer selection uses the message type declared at the call site, not the
concrete type of the instance. The three-argument overload passes this declared type to
`canSerialize`. If more than one registration returns `true`, the serializer registered
later is used. If no registration matches, the JSON serializer is used. When multiple
fallback serializers are registered with the two-argument overload, the later registration
also replaces the earlier one.

The registry doesn't change after startup. Send-selection results are stored for up to
1,024 declared types. Reaching the limit doesn't evict existing entries. Each type first
seen afterward is re-evaluated against the registration list on every send, and its result
isn't stored.

The receive path performs an exact serializer lookup using the canonical content type from
the wire. An unregistered or noncanonical value isn't reinterpreted as JSON and completes
with `ZLinkFrameworkErrorKind.ProtocolError`.

```csharp
public interface IZLinkCodecExtension
{
    void Register(IZLinkCodecRegistrar codecs);
}

public interface IZLinkCodecRegistryBuilder
{
    void Use(IZLinkCodecExtension extension);
}

public interface IZLinkCodecRegistrar
{
    void AddSerializer(
        string contentType,
        IZLinkMessageSerializer serializer);
    void AddSerializer(
        string contentType,
        IZLinkMessageSerializer serializer,
        Func<Type, bool> canSerialize);
}

public interface IZLinkMessageSerializer
{
    ZLinkEncodedPayload Serialize(object value, Type type);
    object? Deserialize(ZLinkEncodedPayload payload, Type type);
}

public readonly struct ZLinkEncodedPayload : IEquatable<ZLinkEncodedPayload>
{
    public ReadOnlyMemory<byte> Bytes { get; }
    public static ZLinkEncodedPayload From(byte[] bytes);
    public static ZLinkEncodedPayload From(ReadOnlyMemory<byte> bytes);
    public static ZLinkEncodedPayload From(ReadOnlySpan<byte> bytes);
    public byte[] ToArray();
    public bool Equals(ZLinkEncodedPayload other);
    public override bool Equals(object? obj);
    public override int GetHashCode();
    public static bool operator ==(
        ZLinkEncodedPayload left,
        ZLinkEncodedPayload right);
    public static bool operator !=(
        ZLinkEncodedPayload left,
        ZLinkEncodedPayload right);
}
```

The official codec package's `Default` is a shared instance with no
separate state. The two classes together implement the Framework
server's codec extension and the Stream Connector's typed payload codec.

```csharp
public sealed class ZLinkMessagePackCodec :
    IZLinkCodecExtension,
    IZlinkStreamPayloadCodec,
    IZlinkStreamCodecRegistration
{
    public static ZLinkMessagePackCodec Default { get; }

    public void Register(IZLinkCodecRegistrar codecs);
    public ZlinkStreamEncodedPayload Encode<TPayload>(TPayload payload);
    public TPayload Decode<TPayload>(ZlinkStreamEncodedPayload payload);
}

public sealed class ZLinkProtobufCodec :
    IZLinkCodecExtension,
    IZlinkStreamPayloadCodec,
    IZlinkStreamCodecRegistration
{
    public static ZLinkProtobufCodec Default { get; }

    public void Register(IZLinkCodecRegistrar codecs);
    public ZlinkStreamEncodedPayload Encode<TPayload>(TPayload payload);
    public TPayload Decode<TPayload>(ZlinkStreamEncodedPayload payload);
}
```
