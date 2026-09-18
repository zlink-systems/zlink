# C++ Stream Connector

The guide to the C++ STREAM client connector. It targets natively built game
engines (Unreal, Godot, Cocos), desktop and server applications, and e2e test tools.

| Order | Document | Content |
|----|------|------|
| 1 | [Stream Connector Overview](01-overview.en.md) | What it is for and where it runs, and its boundary with the server framework |
| 2 | [Installation and the First Connection](02-getting-started.en.md) | Installing the package, the smallest connection, a first send and receive |
| 3 | [Connector Options](03-connector-options.en.md) | The options, their defaults, and when a value is checked |
| 4 | [Sending Packets](04-sending.en.md) | send and request, how a packet name is settled, codecs |
| 5 | [Receiving Packets](05-receiving.en.md) | Registering and unregistering, dispatch mode, the receive queue and its count |
| 6 | [Connection Lifecycle](06-lifecycle.en.md) | Connection state, reconnecting, heartbeat, the close reason |
| 7 | [Error Handling](07-error-handling.en.md) | The closed set of error codes and how each language delivers them |
| 8 | [e2e Client](08-e2e-client.en.md) | The exception-based adapter used by tests and tools |
| 9 | [Engine Adapters](09-engine-adapters.en.md) | The Unreal, Godot and Cocos adapters |
| 10 | [Packaging](10-packaging.en.md) | Build configuration and the published artifacts |
| 11 | [Performance](11-performance.en.md) | Where to measure and what can be tuned |

The file number identifies the same chapter in every language. Chapters 1 to 7 are
shared across all five.

## Related Documents

- Public contract: [C++ public contract](../../../common/spec/stream-connector/languages/cpp/03-stream-connector.en.md)
- Server guide: [C++ server guide](../server/README.en.md)
