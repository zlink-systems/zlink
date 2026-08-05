[Python Binding Specification](README.en.md) · [Bindings Policy](../README.en.md)

# Python Codec Package Policy

Python bindings do not provide JSON, Protobuf, or MessagePack codec packages.
The core `zlink` package exposes raw `Message` and byte payload APIs only.

Applications that need object serialization must keep that choice in their own
application layer or in a future framework package. Do not add replacement
codec packages under the Python binding tree.

This keeps the Python binding contract focused on the low-level protocol API.
Framework-owned codec extensions may be added later only if Python becomes a
framework target under `framework/languages/python/`; they must not revive the
removed bindings codec packages.
