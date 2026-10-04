[English](./core-1.14.0.md) | [한국어](./core-1.14.0.ko.md)

# libzlink 1.14.0 release notes

## Changes

- Classifies `ETERM` from a terminating pipe as a connection error when processing received DATA (#1434).
- Sends terminal ERROR frames after the active asynchronous write completes. Failed or cancelled writes close the connection without another write, preventing WS/WSS write-buffer use-after-free (#1434).
- Closes connections on handshake timeout while a terminal ERROR frame is pending (#1434).
- Detaches the engine before notifying the session of an error and returns after session termination, preventing access to destroyed sessions (#1434).
