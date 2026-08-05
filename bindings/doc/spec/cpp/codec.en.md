[C++ Binding Specification](README.en.md) · [Bindings Policy](../README.en.md)

# C++ Codec Package Policy

C++ bindings do not provide JSON, Protobuf, or MessagePack codec packages. The
core C++ binding exposes raw `message_t` and byte payload APIs only.

Applications that need framework-level serialization should use framework codec
extension packages under `framework/languages/cpp/extensions/`, such as
`framework-codec-protobuf` or `framework-codec-messagepack`. Do not add
replacement codec packages under the C++ binding tree.

This keeps the C++ binding contract focused on the low-level protocol API.
Codec selection, packet-name resolution, serializer lookup, and typed
request/reply policy belong to the framework layer.
