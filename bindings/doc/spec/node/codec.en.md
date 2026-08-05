[Node Binding Specification](README.en.md) · [Bindings Policy](../README.en.md)

# Node Codec Package Policy

Node bindings do not provide JSON, Protobuf, or MessagePack codec packages. The
root `@zlink-systems/zlink` package exposes raw `Message` and byte payload APIs
only.

Applications that need framework-level serialization should use framework codec
extension packages under `framework/languages/node/packages/`, such as
`@zlink-systems/framework-codec-protobuf` or
`@zlink-systems/framework-codec-msgpack`. Do not add replacement codec packages
under the Node binding tree.

This keeps the Node binding contract focused on the low-level protocol API.
Codec selection, packet-name resolution, serializer lookup, and typed
request/reply policy belong to the framework layer.
