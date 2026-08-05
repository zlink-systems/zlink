using System.Diagnostics.Metrics;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal static class ZlinkStreamRuntimeMetrics
{
    // Every record method contains the listener boundary. Metrics callbacks
    // are external observation code and must never escape into connector flow.
    internal const string MeterName = "zlink.framework.stream_connector";

    private static readonly Meter Meter = new(MeterName);

    private static readonly Counter<long> Reconnects =
        Meter.CreateCounter<long>("zlink.stream.reconnects", "{reconnect}");

    internal static void RecordReconnect(
        string transport,
        string outcome,
        string reason)
    {
        try
        {
            if (!Reconnects.Enabled) return;
            Reconnects.Add(
                1,
                new KeyValuePair<string, object?>("transport", transport),
                new KeyValuePair<string, object?>("outcome", outcome),
                new KeyValuePair<string, object?>("reason", reason));
        }
        catch
        {
        }
    }

    internal static string TransportLabel(ZlinkStreamConnectorOptions options)
    {
        return options.Endpoint.Scheme switch
        {
            "tcp" => "tcp",
            "tls" => "tls",
            "ws" => "ws",
            "wss" => "wss",
            _ => "unknown"
        };
    }
}
