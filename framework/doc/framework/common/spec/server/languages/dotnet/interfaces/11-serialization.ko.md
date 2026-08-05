# .NET codec extension 공개 인터페이스

[.NET exact interface 목차](README.ko.md) · [공통 메시지 계약](../../../../04-message-model.ko.md) ·
[공통 Framework API](../../../../06-framework-api.ko.md#9-codec)

## 1. 범위

이 문서는 application이 codec extension을 등록할 때 사용하는 C# API와 외부 codec provider가 구현하는 SPI만
고정한다. JSON 기본 동작, packet name 결정, global object reference의 JSON 형식과 payload ownership은 공통
메시지 계약이 소유한다. 내부 registry, codec 선택 cache와 dispatch 구현은 이 문서의 계약이 아니다.

JSON만 사용하는 application은 codec API를 호출하지 않는다. 선택 codec package를 사용하는 application은
`IZLinkCodecRegistryBuilder.Use(...)`에 extension instance 하나를 등록한다. `IZLinkCodecRegistrar`,
`IZLinkMessageSerializer`와 `ZLinkEncodedPayload`는 custom codec provider가 구현할 때만 사용하는 SPI다.

## 2. Codec 등록 API와 provider SPI

`IZLinkCodecRegistrar`는 business payload serializer만 등록한다. STREAM header의 codec 값은
Stream Connector가 소유하는 `IZlinkStreamCodecRegistration`으로 선언한다. 이 분리로 HTTP client가
STREAM runtime이나 compression package에 의존하지 않는다.

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

공식 codec package의 `Default`는 별도 상태를 갖지 않는 공유 instance다. 두 class는 Framework server의 codec
extension과 Stream Connector의 typed payload codec을 함께 구현한다.

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
