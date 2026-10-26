using System.Net;
using System.Net.Sockets;
using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;

[CollectionDefinition(Name, DisableParallelization = true)]
public sealed class DefaultProxyCollection
{
    public const string Name = "Process default proxy";
}

[Collection(DefaultProxyCollection.Name)]
public sealed class DirectConnectionTests
{
    [Fact]
    public async Task WebSocketConnectIgnoresProcessDefaultProxy()
    {
        using var proxy = new TcpListener(IPAddress.Loopback, 0);
        proxy.Start();
        using var stopProxy = new CancellationTokenSource();
        var proxyConnections = 0;
        var proxyTask = Task.Run(async () =>
        {
            try
            {
                while (true)
                {
                    using var connection = await proxy.AcceptTcpClientAsync(stopProxy.Token);
                    Interlocked.Increment(ref proxyConnections);
                    await connection
                        .GetStream()
                        .WriteAsync(
                            Encoding.ASCII.GetBytes(
                                "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n"
                            ),
                            stopProxy.Token
                        );
                }
            }
            catch (OperationCanceledException) when (stopProxy.IsCancellationRequested) { }
        });
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        await using var app = builder.Build();
        app.UseWebSockets();
        var releaseServer = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        app.Map(
            "/ws",
            async (HttpContext context) =>
            {
                using var socket = await context.WebSockets.AcceptWebSocketAsync();
                await releaseServer.Task;
            }
        );
        await app.StartAsync();
        var endpoint = new Uri(app.Urls.Single());
        var proxyEndpoint = (IPEndPoint)proxy.LocalEndpoint;
        var originalProxy = HttpClient.DefaultProxy;
        try
        {
            HttpClient.DefaultProxy = new WebProxy(
                new Uri($"http://127.0.0.1:{proxyEndpoint.Port}"),
                false
            );
            await using var connector = ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri($"ws://127.0.0.1:{endpoint.Port}/ws"),
                    Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                    Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                }
            );
            var failure = await Record.ExceptionAsync(async () => await connector.Connect.Async());
            Assert.Equal(0, Volatile.Read(ref proxyConnections));
            Assert.Null(failure);
        }
        finally
        {
            HttpClient.DefaultProxy = originalProxy;
            releaseServer.TrySetResult();
            stopProxy.Cancel();
            await proxyTask;
        }
    }
}
