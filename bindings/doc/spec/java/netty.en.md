[Java Binding Specification](README.en.md) · [Bindings Policy](../README.en.md)

# Java Netty Extension Specification

This document defines the public contract for the Java Netty `ByteBuf`
extension. The adapter is separate from the core binding so Netty-specific
entrypoints do not become part of `systems.zlink`, and applications
opt in to the Netty dependency explicitly.

## Artifact And Package

- Maven `zlink-ext-netty`
- `systems.zlink.netty`

## Ownership Rules

- `from(ByteBuf)` copies the readable bytes between `readerIndex` and
  `writerIndex`.
- `from(ByteBuf)` must not change `readerIndex` or `writerIndex`.
- The extension must not call `retain()` or `release()` on caller-owned
  `ByteBuf`.
- `copyTo(Message, ByteBuf)` copies the full message at the current
  `writerIndex`.
- `copyTo(Message, ByteBuf)` may advance `writerIndex` by the copied byte
  count, but must not change `readerIndex`.
- `copyTo(Message, ByteBuf)` must not call `retain()` or `release()`.

## API

```java
package systems.zlink.netty;

public final class NettyMessages {
    public static systems.zlink.Message from(
        io.netty.buffer.ByteBuf source);

    public static int copyTo(
        systems.zlink.Message message,
        io.netty.buffer.ByteBuf destination);
}
```
