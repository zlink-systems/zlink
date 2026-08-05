// Verifies OBS-B1 Connection Metrics behavior.
using System.Diagnostics.Metrics;
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsB1ConnectionMetricsScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var connectors = new List<IZlinkStreamConnector>();
        try
        {
            for (var index = 0; index < 3; index++)
            {
                var connector = await context.ConnectAsync();
                await connector.Request(new AuthenticateReq($"obs-b1-{index}-{Guid.NewGuid():N}"))
                    .Async<AuthenticateRes>();
                connectors.Add(connector);
            }
            var active = (await context.Session.Post("/metrics/wait")
                .Body(new MetricWaitReq("zlink.stream.connections.active", 3))
                .Async<MetricSample[]>()).Body;
            ZlinkStreamAssert.Ensure(active.Any(sample => sample.Name == "zlink.stream.connections.active"
                                                         && sample.Value == 3),
                "OBS-B1 active connection gauge did not reach three.");
            foreach (var connector in connectors) await connector.Close.Async();
            foreach (var connector in connectors) await connector.DisposeAsync();
            connectors.Clear();
            var closed = (await context.Session.Post("/metrics/wait")
                .Body(new MetricWaitReq("zlink.stream.connections.closed", 3))
                .Async<MetricSample[]>()).Body;
            ZlinkStreamAssert.Ensure(closed.Any(sample => sample.Name == "zlink.stream.connections.closed"
                                                         && sample.Value >= 3),
                "OBS-B1 closed connection counter did not increase by three.");
            var inactive = (await context.Session.Post("/metrics/wait")
                .Body(new MetricWaitReq("zlink.stream.connections.active", 0, 0))
                .Async<MetricSample[]>()).Body;
            ZlinkStreamAssert.Ensure(inactive.Any(sample => sample.Name == "zlink.stream.connections.active"
                                                           && sample.Value == 0),
                "OBS-B1 active connection gauge did not return to zero.");
        }
        finally
        {
            foreach (var connector in connectors) await connector.DisposeAsync();
        }

        long reconnectAttempts = 0;
        using var listener = new MeterListener
        {
            InstrumentPublished = (instrument, owner) =>
            {
                // The reconnect counter belongs to the client connector, which
                // publishes it on its own meter; the common connector spec §6.2
                // pins the instrument name and labels, not the meter name.
                if (instrument.Name == "zlink.stream.reconnects")
                    owner.EnableMeasurementEvents(instrument);
            }
        };
        listener.SetMeasurementEventCallback<long>((_, value, _, _) =>
            Interlocked.Add(ref reconnectAttempts, value));
        listener.Start();
        await using var proxy = new ReconnectProxy(new Uri(context.Options.SessionEndpoint));
        await using var reconnecting = await context.ConnectAsync(proxy.Endpoint.ToString(), persistentReconnect: true);
        await proxy.WaitForConnectionAsync();
        var sawReconnect = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var reconnectingStateSeen = 0;
        reconnecting.ConnectionStateChanged += (change, _) =>
        {
            if (change.Current == ZlinkStreamConnectionState.Reconnecting)
                Volatile.Write(ref reconnectingStateSeen, 1);
            else if (change.Current == ZlinkStreamConnectionState.Connected
                     && Volatile.Read(ref reconnectingStateSeen) != 0)
                sawReconnect.TrySetResult();
            return ValueTask.CompletedTask;
        };
        proxy.DropConnection();
        await proxy.WaitForConnectionAsync();
        await sawReconnect.Task.WaitAsync(TimeSpan.FromSeconds(10));
        // The connector publishes the Connected state before it records the
        // reconnect, and it must - a metric listener may never gate connection
        // state (connector spec §6.2). So seeing Connected does not mean the
        // counter has been written yet; wait for it instead of racing it.
        var metricDeadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        while (Interlocked.Read(ref reconnectAttempts) == 0
               && DateTimeOffset.UtcNow < metricDeadline)
            await Task.Delay(50);
        ZlinkStreamAssert.Ensure(Interlocked.Read(ref reconnectAttempts) > 0,
            "OBS-B1 connector reconnect counter did not increase.");
        await reconnecting.Close.Async();
        Console.WriteLine("scenario OBS-B1 passed");
    }
}
