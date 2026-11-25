#nullable enable
using System;
using System.Threading;
using System.Threading.Tasks;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

public static class ConnectorExample
{
    public static async Task RunAsync(Uri endpoint, Action<string> report)
    {
        var timeout = TimeSpan.FromSeconds(5);
        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = endpoint,
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                RequestTimeout = timeout,
            }
        );

        await connector.Connect.Async();
        report("Connected");

        await connector.Send(new Publish()).Async();
        var notice = await connector.WaitFor<Notice>().Timeout(timeout).Async();
        if (notice.Payload.Text != "Hello from the server")
            throw new InvalidOperationException("Unexpected Notice");
        report("Push: " + notice.Payload.Text);

        // Observe the connector's next Connected event after the server drops this session.
        var reconnected = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using (
            connector.OnConnectionStateChanged(
                (change, _) =>
                {
                    if (change.Current == ZlinkStreamConnectionState.Connected)
                        reconnected.TrySetResult(true);
                    return ValueTask.CompletedTask;
                }
            )
        )
        using (var deadline = new CancellationTokenSource(timeout))
        using (deadline.Token.Register(() => reconnected.TrySetCanceled()))
        {
            await connector.Send(new Reconnect()).Async();
            await reconnected.Task;
        }
        report("Reconnected");

        // The typed codec encodes long as a decimal string. The optional label is omitted.
        var echoed = await connector.Request(new Echo(long.MaxValue, null)).Async<Echoed>();
        if (echoed.Score != long.MaxValue || echoed.Nickname != null || echoed.Label != "default")
            throw new InvalidOperationException("Unexpected Echoed");
        report($"Reply: {echoed.Score}, nickname=null, label={echoed.Label}");

        var left = await connector.Request(new Leave()).Async<Left>();
        if (!left.Accepted)
            throw new InvalidOperationException("Leave was rejected");
        report("Leave acknowledged");
        await connector.Close.Async();
        report("Closed");
    }

    public sealed class Publish { }

    public sealed class Reconnect { }

    public sealed class Leave { }

    public sealed class Notice
    {
        public Notice(string text) => Text = text;

        public string Text { get; }
    }

    public sealed class Echo
    {
        public Echo(long score, string? nickname)
        {
            Score = score;
            Nickname = nickname;
        }

        public long Score { get; }
        public string? Nickname { get; }
    }

    public sealed class Echoed
    {
        public Echoed(long score, string? nickname, string label)
        {
            Score = score;
            Nickname = nickname;
            Label = label;
        }

        public long Score { get; }
        public string? Nickname { get; }
        public string Label { get; }
    }

    public sealed class Left
    {
        public Left(bool accepted) => Accepted = accepted;

        public bool Accepted { get; }
    }
}
