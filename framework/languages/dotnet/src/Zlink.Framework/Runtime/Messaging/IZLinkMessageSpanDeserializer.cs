namespace Zlink.Framework.Runtime.Messaging;

internal interface IZLinkMessageSpanDeserializer
{
    object? Deserialize(ReadOnlySpan<byte> payload, Type type);
}
