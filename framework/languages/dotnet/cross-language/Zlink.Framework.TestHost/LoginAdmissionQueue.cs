using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;

internal static class LoginAdmissionQueue
{
    public const string StreamName = "login";
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(30);

    private static string PacketName => LoginAdmission.Setting("ZLINK_LOGIN_QUEUE_PACKET");
    private static string Role => LoginAdmission.Setting("ZLINK_LOGIN_QUEUE_ROLE");

    public static void Configure(IServiceCollection services, TestHostOptions options)
    {
        if (!string.IsNullOrWhiteSpace(options.EventFilePath))
            services.AddSingleton(new TestHostMessageFlowListener(options.EventFilePath + ".flow"));

        services.AddZLinkFramework(framework =>
        {
            if (!string.IsNullOrWhiteSpace(options.EventFilePath))
                framework.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);

            var endpoint = Require(options.RedisEndpoint, "--redis-endpoint");
            var prefix = Require(options.RedisKeyPrefix, "--redis-key-prefix");
            framework.AddLocationStore(
                new ZLinkRedisLocationStore(store =>
                {
                    store.ConnectionString = endpoint;
                    store.KeyPrefix = $"{prefix}:location";
                })
            );
            framework.AddRelocationStore(
                new ZLinkRedisRelocationStore(store =>
                {
                    store.ConnectionString = endpoint;
                    store.KeyPrefix = $"{prefix}:relocation";
                })
            );

            var role = Role;
            var objects = framework
                .AddRouteMesh(Require(options.MeshName, "--mesh-name"))
                .Listen(Require(options.BindEndpoint, "--bind-endpoint"))
                .SetRoutingIdPrefix(role)
                .Objects();
            switch (role)
            {
                case "object-server":
                    objects.Server();
                    break;
                case "object-client":
                    objects.Client();
                    break;
                default:
                    throw new InvalidOperationException($"Unexpected login queue role '{role}'.");
            }

            var stream = framework.AddStreamNode(StreamName);
            stream.Bind(Require(options.StreamEndpoint, "--stream-endpoint"));
            stream.AddSession<Session>();
            stream.AuthenticationPacketName = PacketName;
            stream.MaxConcurrentAuthentications = LoginAdmission.Number(
                "ZLINK_LOGIN_QUEUE_LOCAL_LIMIT"
            );
            stream.AdmissionQueueEnabled = true;
            stream.AdmissionQueueKey = LoginAdmission.Setting("ZLINK_LOGIN_QUEUE_KEY");
            stream.AdmissionGlobalLimit = LoginAdmission.Number("ZLINK_LOGIN_QUEUE_GLOBAL_LIMIT");
        });
    }

    public static async Task ConnectAsync(TestHostOptions options)
    {
        var firstEndpoint = Require(options.StreamEndpoint, "--stream-endpoint");
        var peerEndpoint = Require(options.PeerStreamEndpoint, "--peer-stream-endpoint");
        await using var held = CreateConnector(firstEndpoint);
        await using var second = CreateConnector(peerEndpoint);
        await using var third = CreateConnector(peerEndpoint);

        await held.Connect.Async();
        await second.Connect.Async();
        await third.Connect.Async();
        using var secondPosition = ObservePosition(second, "second");
        using var thirdPosition = ObservePosition(third, "third");

        var first = RequestAsync(held, "held");
        await WaitForMarkerAsync("held", CancellationToken.None, first);
        await Task.WhenAll(first, RequestAsync(second, "second"), RequestAsync(third, "third"));
    }

    private static IZlinkStreamConnector CreateConnector(string endpoint) =>
        ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(endpoint),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                RequestTimeout = Timeout,
            }
        );

    private static IDisposable ObservePosition(IZlinkStreamConnector client, string name) =>
        client.OnAdmissionPosition(
            (value, _) =>
            {
                if (value.Position < 1 || value.EstimatedWaitMs < 0)
                    throw new InvalidOperationException($"Invalid admission position for {name}.");
                Append(
                    $"{name}.position",
                    FormattableString.Invariant($"{value.Position}|{value.EstimatedWaitMs}\n")
                );
                return ValueTask.CompletedTask;
            }
        );

    private static async Task RequestAsync(IZlinkStreamConnector client, string value)
    {
        var reply = await client.Request(value).PacketName(PacketName).Async<string>();
        if (reply != value)
            throw new InvalidOperationException($"Unexpected login reply '{reply}' for {value}.");
        Console.WriteLine($"admitted {value}");
    }

    private static async Task WaitForMarkerAsync(
        string name,
        CancellationToken cancellationToken,
        Task? pending = null
    )
    {
        var path = Marker(name);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(Timeout);
        while (!File.Exists(path))
        {
            if (pending is { IsCompleted: true })
            {
                await pending;
                throw new InvalidOperationException($"Request completed before {name}.");
            }
            await Task.Delay(10, deadline.Token);
        }
    }

    private static string Require(string? value, string option) =>
        value ?? throw new InvalidOperationException($"Missing {option}.");

    private static string Marker(string name) =>
        Path.Combine(LoginAdmission.Setting("ZLINK_LOGIN_QUEUE_DIR"), name);

    private static void Append(string name, string value) =>
        File.AppendAllText(Marker(name), value);

    internal sealed class Session(IZLinkSessionContext context) : IZLinkSession
    {
        public IZLinkSessionContext Context => context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken
        )
        {
            if (dispatch.PacketName != PacketName)
                throw new InvalidOperationException($"Unexpected packet '{dispatch.PacketName}'.");

            var value = payload.Decode<string>();
            var role = Role;
            Append("handlers.events", $"enter|{role}|{value}\n");
            try
            {
                if (value == "held")
                {
                    Append("held", "");
                    await WaitForMarkerAsync("release", cancellationToken);
                }
                await context.Client.Reply(value).Async();
            }
            finally
            {
                Append("handlers.events", $"leave|{role}|{value}\n");
            }
        }
    }
}
