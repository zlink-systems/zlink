# Node/TypeScript Stream Connector

The guide to the TypeScript STREAM client connector. **The browser is its main
target**, and Unity WebGL and Godot Web builds use it. Node.js is not this
connector's product runtime; it is used by tests and tools.

| Order | Document | Content |
|----|------|------|
| 1 | [Stream Connector Overview](01-overview.en.md) | What it is for and where it runs, and its boundary with the server framework |
| 2 | [Installation and the First Connection](02-getting-started.en.md) | Installing the package, the smallest connection, a first send and receive |
| 3 | [Connector Options](03-connector-options.en.md) | The options, their defaults, and when a value is checked |
| 4 | [Sending Packets](04-sending.en.md) | send and request, how a packet name is settled, codecs |
| 5 | [Receiving Packets](05-receiving.en.md) | Registering and unregistering, dispatch mode, the receive queue and its count |
| 6 | [Connection Lifecycle](06-lifecycle.en.md) | Connection state, reconnecting, heartbeat, the close reason |
| 7 | [Error Handling](07-error-handling.en.md) | The closed set of error codes and how each language delivers them |
| 8 | [Browser](08-browser.en.md) | Using it in a browser and the WebSocket constraints |
| 9 | [Unity WebGL](09-unity-webgl.en.md) | Using it from a Unity WebGL build |
| 10 | [Game Engine Integration](12-engine-integration.en.md) | Connector choice by engine and the Engine Lobby sample |

The file number identifies the same chapter in every language. Chapters 1 to 7 are
shared across all five.

## Related Documents

- Public contract: [Node/TypeScript public contract](../../../common/spec/stream-connector/languages/typescript/03-stream-connector.en.md)
- Server guide: [Node/TypeScript server guide](../server/README.en.md)
