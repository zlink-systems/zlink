# Kotlin Stream Connector

The guide to the Kotlin STREAM client connector. It wraps the Java connector in
coroutine idiom and targets server applications and test tools.

| Order | Document | Content |
|----|------|------|
| 1 | [Stream Connector Overview](01-overview.en.md) | What it is for and where it runs, and its boundary with the server framework |
| 2 | [Installation and the First Connection](02-getting-started.en.md) | Installing the package, the smallest connection, a first send and receive |
| 3 | [Connector Options](03-connector-options.en.md) | The options, their defaults, and when a value is checked |
| 4 | [Sending Packets](04-sending.en.md) | send and request, how a packet name is settled, codecs |
| 5 | [Receiving Packets](05-receiving.en.md) | Registering and unregistering, dispatch mode, the receive queue and its count |
| 6 | [Connection Lifecycle](06-lifecycle.en.md) | Connection state, reconnecting, heartbeat, the close reason |
| 7 | [Error Handling](07-error-handling.en.md) | The closed set of error codes and how each language delivers them |

The file number identifies the same chapter in every language. Chapters 1 to 7 are
shared across all five.

## Related Documents

- Public contract: [Kotlin public contract](../../../common/spec/stream-connector/languages/java/03-stream-connector.en.md)
- Server guide: [Kotlin server guide](../server/README.en.md)
