[Go Binding Specification](README.en.md) · [Bindings Policy](../README.en.md)

# Go Codec Package Policy

Go bindings do not provide JSON, Protobuf, or MessagePack codec modules. The
root `zlink.systems/zlink` module exposes raw `Message` and byte payload APIs
only.

Applications that need object serialization must keep that choice in their own
application layer or in a future framework module. Do not add replacement
codec modules under the Go binding tree.

This keeps the Go binding contract focused on the low-level protocol API.
Framework-owned codec extensions may be added later only if Go becomes a
framework target under `framework/languages/go/`; they must not revive the
removed bindings codec modules.
