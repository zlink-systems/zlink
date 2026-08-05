namespace Zlink.Framework.Contracts.Messaging;

public sealed partial class ZLinkMessage
{
    private const string DefaultContentType = "application/json";

    public string? ContentType { get; }

    public ZlinkStreamCodec? StreamCodec { get; }

    public bool IsEmpty => _declaredType is null && _payload.IsEmpty;

    public static ZLinkMessage Empty { get; } =
        new(ReadOnlyMemory<byte>.Empty, DefaultContentType, null, null);

    public static ZLinkMessage From<T>(T value)
    {
        return new ZLinkMessage(value, typeof(T));
    }

    public T Decode<T>()
    {
        if (_declaredType is not null)
        {
            if (_value is T typed) return typed;

            if (_value is null) return default!;
        }

        var decoded = Decode(typeof(T));
        return decoded is null ? default! : (T)decoded;
    }
}
