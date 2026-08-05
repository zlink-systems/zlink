[Rust Binding Specification](README.en.md) · [Bindings Policy](../README.en.md)

# Rust Codec Crate Policy

Rust bindings do not provide JSON, Protobuf, or MessagePack codec crates. The
core `zlink` crate exposes raw `Message` and byte payload APIs only.

Applications that need object serialization must keep that choice in their own
application layer or in a future framework crate. Do not add replacement codec
crates under the Rust binding tree.

This keeps the Rust binding contract focused on the low-level protocol API.
Framework-owned codec extensions may be added later only if Rust becomes a
framework target under `framework/languages/rust/`; they must not revive the
removed bindings codec crates.
