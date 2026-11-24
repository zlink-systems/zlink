using System;

var endpoint =
    Environment.GetEnvironmentVariable("CONNECTOR_EXAMPLE_ENDPOINT") ?? "ws://127.0.0.1:7301";
await ConnectorExample.RunAsync(new Uri(endpoint), Console.WriteLine);
