using System.Globalization;
using Systems.Zlink;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Messaging;

internal static class LoginAdmission
{
    public static string Setting(string name) =>
        Environment.GetEnvironmentVariable(name)
        ?? throw new InvalidOperationException($"Missing login admission setting {name}.");

    public static string Marker(string name) => Path.Combine(Setting("ZLINK_LOGIN_DIR"), name);

    public static int Number(string name) => int.Parse(Setting(name), CultureInfo.InvariantCulture);

    private static TimeSpan Timeout => TimeSpan.FromMilliseconds(Number("ZLINK_LOGIN_TIMEOUT_MS"));

    public static async Task WaitForAsync(string name, CancellationToken cancellationToken)
    {
        var path = Marker(name);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(Timeout);
        while (!File.Exists(path))
            await Task.Delay(5, timeout.Token);
    }

    public static async Task ConnectAsync(string endpoint)
    {
        var options = new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            RequestTimeout = Timeout,
        };
        await using var held = ZlinkStreamConnectorFactory.Create(options);
        await using var contender = ZlinkStreamConnectorFactory.Create(options);
        await held.Connect.Async();
        await contender.Connect.Async();
        using var waiting = contender.OnAdmissionWaiting(
            (value, _) =>
            {
                File.AppendAllText(
                    Marker("waiting"),
                    FormattableString.Invariant(
                        $"{value.RequestPacketName}|{(long)value.RetryAfter.TotalMilliseconds}\n"
                    )
                );
                return ValueTask.CompletedTask;
            }
        );

        var packet = Setting("ZLINK_LOGIN_PACKET");
        await RequestAsync(contender, "ZLINK_LOGIN_CONTROL");
        var pending = RequestAsync(held, "ZLINK_LOGIN_HELD");
        await WaitForAsync("held", CancellationToken.None);
        await Task.WhenAll(pending, RequestAsync(contender, "ZLINK_LOGIN_CONTENDER"));

        async Task RequestAsync(IZlinkStreamConnector client, string name)
        {
            var expected = Setting(name);
            var reply = await client.Request(expected).PacketName(packet).Async<string>();
            if (reply != expected)
                throw new InvalidOperationException($"Unexpected login reply '{reply}'.");
        }
    }
}

internal sealed class LoginAdmissionSession(IZLinkSessionContext context) : IZLinkSession
{
    public IZLinkSessionContext Context => context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken
    )
    {
        var value = payload.Decode<string>();
        File.AppendAllText(
            LoginAdmission.Marker("server.events"),
            $"{value}|{context.SessionId}\n"
        );
        if (value == LoginAdmission.Setting("ZLINK_LOGIN_HELD"))
        {
            File.WriteAllText(LoginAdmission.Marker("held"), "");
            await LoginAdmission.WaitForAsync("release", cancellationToken);
        }
        await context.Client.Reply(value).Async();
    }
}
