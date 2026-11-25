# Connector example guidelines

Start the common server and run a client using [README.md](README.md).
The server dispatches packets in `server/server.ts`. Define message values in `server/messages.ts`.
The build generates the Framework JSON schemas from those declarations.

- Keep packet names and JSON fields aligned across the three clients. Use public connector APIs
  and the default typed JSON codec.
- Keep the TypeScript negative cases for a numeric `score`, an omitted `nickname`, and a null
  `label`. Java and C# encode their typed `long` values automatically.
- Observe the reconnect event and wait for the `Left` response before closing the connector.
  The `Reconnect` message closes the current demo session so the connector can demonstrate
  automatic reconnection.
- Preserve the first failing client's output and the server's `.logs/run-*/server.log` when
  reporting a failure. Include the published package versions from `TEMPLATE_VERSIONS`.

[Message value rules](https://docs.zlink.systems/node/guide/server/20-channel-messaging/#12-message-value-rules)
