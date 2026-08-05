[Java Binding Specification](README.en.md) · [Bindings Policy](../README.en.md)

# Java Codec Artifact Policy

Java bindings do not provide JSON, Protobuf, or MessagePack codec artifacts.
The core Java binding exposes raw `Message` and byte payload APIs only.

Applications that need framework-level serialization should use framework codec
extension artifacts under `framework/languages/java/`, such as
`zlink-framework-codec-protobuf` or `zlink-framework-codec-msgpack`. Do not add
replacement codec artifacts under the Java binding tree.

This keeps the Java binding contract focused on the low-level protocol API.
Codec selection, packet-name resolution, serializer lookup, and typed
request/reply policy belong to the framework layer.
