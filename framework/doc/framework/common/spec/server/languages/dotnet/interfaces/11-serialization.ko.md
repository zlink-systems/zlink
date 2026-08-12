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

`contentType`에는 parameter가 없는 ASCII `type/subtype`을 전달한다. Registry는 startup에 값
앞뒤의 SP와 TAB을 제거하고 ASCII 대문자를 소문자로 바꾼다. 이 결과가 registry에서 사용하는
canonical content-type이다. Parameter, 값 내부의 공백 또는 non-ASCII token이 있으면
`ArgumentException`을 발생시킨다. 같은 canonical content-type을 다시 등록하면 마지막
serializer가 앞의 serializer를 교체한다.

Framework service wire에서 받은 값은 이미 canonical content-type이어야 한다. 다른 표기의 값은
`ZLinkFrameworkErrorKind.ProtocolError`로 완료한다. HTTP client는 response parameter를 먼저
parse하고, parameter가 없는 media type만 이 규칙에 전달한다.

송신 serializer를 고를 때는 실제 instance의 concrete type이 아니라 호출 지점에 선언된 message
type을 사용한다. 세 번째 인자를 받는 overload는 이 declared type을 `canSerialize`에 전달한다.
둘 이상의 등록이 `true`를 반환하면 나중에 등록한 serializer를 사용한다. 조건에 맞는 등록이
없으면 JSON serializer를 사용한다. 두 인자 overload로 fallback serializer를 여러 개 등록한
경우에도 나중 등록이 앞의 등록을 교체한다.

Registry는 startup 뒤 바뀌지 않는다. 송신 선택 결과는 declared type 1,024개까지 저장한다.
한도에 도달해도 기존 entry를 제거하지 않는다. 그 뒤 처음 보는 type은 송신할 때마다 등록
목록을 다시 평가하고 결과를 저장하지 않는다.

수신 경로는 wire에서 받은 canonical content-type으로 serializer를 정확히 찾는다. 등록되지 않은
값이나 canonical form이 아닌 값은 JSON으로 다시 해석하지 않고
`ZLinkFrameworkErrorKind.ProtocolError`로 완료한다.

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
