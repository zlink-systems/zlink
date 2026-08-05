[C Binding Specification](README.en.md) · [Bindings Policy](../README.en.md)

# C Codec Extension Specification

C does not define required codec extension helpers.

The public C binding contract is the installed C API in
`core/include/zlink.h`. Applications that need protobuf, JSON, MessagePack, or
another serializer convert their domain objects to bytes before constructing
`zlink_msg_t`, and parse bytes after receiving `zlink_msg_t`.

The C binding must not add a second public codec layer with helper names such
as `zlink_parse_json(...)` or `zlink_to_protobuf_message(...)` unless that
helper first becomes part of the core public C contract.

This keeps the C surface close to the core API and avoids shallow wrappers
that only forward to an application-owned serializer without hiding ownership,
validation, or lifetime complexity.
