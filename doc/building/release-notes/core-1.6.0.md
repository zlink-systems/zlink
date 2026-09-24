[English](./core-1.6.0.md) | [한국어](./core-1.6.0.ko.md)

# libzlink 1.6.0 release notes

The public C API and ABI are unchanged from 1.5.0
(`LIBZLINK_ABI_SOVERSION=0`).

## What changed

ROUTER no longer rejects reply-token publication with `ECONNABORTED`
(`INTERNAL_ERROR 206`) when the source pipe disconnects after a complete
REQUEST has been received. Physical disconnection does not invalidate the
reply token ([#1051](https://github.com/zlink-systems/zlink/issues/1051)).

## Impact on consumers

Existing Core consumers need no source changes or ABI migration. A ROUTER
receive the complete REQUEST and its reply token even if the source pipe
disconnects after the REQUEST has been received.

## Verification

A regression test terminates the source pipe after the complete REQUEST
is received and checks that ROUTER returns the REQUEST and a reply token.

The release tag is [`core/v1.6.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.6.0).
