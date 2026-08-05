[.NET Binding Specification](README.en.md) · [Bindings Policy](../README.en.md)

# .NET Codec Package Policy

.NET bindings do not provide JSON, Protobuf, or MessagePack codec packages. The
core `Systems.Zlink` assembly exposes raw `Message` and byte payload APIs only.

Applications that need framework-level serialization should use framework codec
extension packages under `framework/languages/dotnet/src/`, such as
`Zlink.Framework.Codecs.Protobuf` or `Zlink.Framework.Codecs.MessagePack`.
Do not add replacement codec packages under the .NET binding tree.

This keeps the .NET binding contract focused on the low-level protocol API.
Codec selection, packet-name resolution, serializer lookup, and typed
request/reply policy belong to the framework layer.
