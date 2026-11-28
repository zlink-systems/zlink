using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class AdmissionQueueConnectorMeshTests
{
    [Fact]
    public async Task PublicConnectorsWaitForOneGlobalPermitAcrossTwoNodes()
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(25));
        var token = deadline.Token;
        using var flow = Environment.GetEnvironmentVariable("ZLINK_ADMISSION_QUEUE_TEST_FLOW")
            is { } path
            ? new TestHostMessageFlowListener(Path.Combine(path, "admission-connector-mesh.flow"))
            : null;
        var location = new ZLinkInMemoryProviderLocationStore();
        var relocation = new InMemoryRelocationStore();
        var probe = new AdmissionQueueMeshTests.HandlerProbe();
        using var server = AdmissionQueueMeshTests.CreateHost(location, relocation, probe, true);
        using var client = AdmissionQueueMeshTests.CreateHost(location, relocation, probe, false);
        await server.StartAsync(token);
        await client.StartAsync(token);
        try
        {
            await using var first = await ConnectAsync(server, token);
            await using var second = await ConnectAsync(client, token);
            var position = new TaskCompletionSource<ZlinkStreamAdmissionPosition>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var queued = 0;
            using var onQueued = second.OnAdmissionQueued(_ =>
            {
                Interlocked.Increment(ref queued);
                return ValueTask.CompletedTask;
            });
            using var onPosition = second.OnAdmissionPosition(
                (value, _) =>
                {
                    position.TrySetResult(value);
                    return ValueTask.CompletedTask;
                }
            );

            try
            {
                var firstLogin = LoginAsync(first, token);
                await WaitForEventAsync(probe.FirstEntered.Task, firstLogin, token);
                var secondLogin = LoginAsync(second, token);
                await WaitForEventAsync(position.Task, secondLogin, token);
                Assert.Equal(1, (await position.Task).Position);
                Assert.Equal(1, Volatile.Read(ref queued));
                Assert.Equal(1, Volatile.Read(ref probe.Entered));
                Assert.Equal(1, Volatile.Read(ref probe.Active));

                probe.ReleaseFirst.TrySetResult();
                var replies = await Task.WhenAll(firstLogin, secondLogin).WaitAsync(token);
                Assert.All(replies, reply => Assert.True(reply.Accepted));
                Assert.Equal(2, Volatile.Read(ref probe.Entered));
                Assert.Equal(1, Volatile.Read(ref probe.Peak));
            }
            finally
            {
                probe.ReleaseFirst.TrySetResult();
            }
        }
        finally
        {
            await client.StopAsync(CancellationToken.None);
            await server.StopAsync(CancellationToken.None);
        }
    }

    private static async Task<IZlinkStreamConnector> ConnectAsync(
        IHost host,
        CancellationToken cancellationToken
    )
    {
        var endpoint = host
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .GetListenerStatus(ZLinkListenerKind.Stream, "login")
            .Endpoint;
        var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(endpoint),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Heartbeat = new() { Enabled = false },
                Reconnect = new() { Enabled = false },
            }
        );
        try
        {
            await connector.Connect.Async(cancellationToken);
            return connector;
        }
        catch
        {
            await connector.DisposeAsync();
            throw;
        }
    }

    private static Task<LoggedIn> LoginAsync(
        IZlinkStreamConnector connector,
        CancellationToken cancellationToken
    ) => connector.Request(new { }).PacketName("Login").Async<LoggedIn>(cancellationToken).AsTask();

    private static async Task WaitForEventAsync(
        Task observed,
        Task login,
        CancellationToken cancellationToken
    )
    {
        var completed = await Task.WhenAny(observed, login).WaitAsync(cancellationToken);
        await completed;
        Assert.Same(observed, completed);
    }

    private sealed record LoggedIn(bool Accepted);
}
