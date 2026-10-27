using System.Diagnostics;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

internal static class RawMeshPeerAdmission
{
    private static readonly TimeSpan ObservationTimeout = TimeSpan.FromSeconds(2);

    internal static async Task<Received> CompleteAsync(
        IDealerSocket source,
        string meshName,
        string sourceEndpoint,
        ulong lifecycleGeneration = 1,
        ulong descriptorRevision = 1
    )
    {
        Received? response = null;
        try
        {
            var sawHello = false;
            for (var record = 0; record < 2; record++)
            {
                var received = await ReceiveAsync(source);
                if (
                    !ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                        received.FirstPart().AsSpan(),
                        out var command,
                        out _,
                        out _
                    )
                )
                {
                    received.Dispose();
                    throw new Xunit.Sdk.XunitException("Expected a route admission record.");
                }
                if (command == ServiceWireConstants.Command.Hello)
                {
                    received.Dispose();
                    Assert.False(sawHello);
                    sawHello = true;
                    await SubmitAdmitAsync(
                        source,
                        meshName,
                        sourceEndpoint,
                        lifecycleGeneration,
                        descriptorRevision
                    );
                }
                else
                {
                    if (command != ServiceWireConstants.Command.Admit || response is not null)
                    {
                        received.Dispose();
                        throw new Xunit.Sdk.XunitException("Expected one HELLO and one ADMIT.");
                    }
                    response = received;
                }
            }
            Assert.True(sawHello);
            return Assert.IsType<Received>(response);
        }
        catch
        {
            response?.Dispose();
            throw;
        }
    }

    internal static async Task AnswerHelloAsync(
        IDealerSocket source,
        string meshName,
        string sourceEndpoint,
        ulong lifecycleGeneration = 1,
        ulong descriptorRevision = 1
    )
    {
        using var hello = await ReceiveAsync(source);
        Assert.True(
            ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                hello.FirstPart().AsSpan(),
                out var command,
                out _,
                out _
            )
        );
        Assert.Equal(ServiceWireConstants.Command.Hello, command);
        await SubmitAdmitAsync(
            source,
            meshName,
            sourceEndpoint,
            lifecycleGeneration,
            descriptorRevision
        );
    }

    private static async Task SubmitAdmitAsync(
        IDealerSocket source,
        string meshName,
        string sourceEndpoint,
        ulong lifecycleGeneration,
        ulong descriptorRevision
    )
    {
        using var admit = Message.From(
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Admit,
                meshName,
                sourceEndpoint,
                lifecycleGeneration,
                descriptorRevision,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server
            )
        );
        await source.Send().Message(admit).Async(CancellationToken.None).Admitted;
    }

    private static async Task<Received> ReceiveAsync(IDealerSocket source)
    {
        var started = Stopwatch.GetTimestamp();
        while (Stopwatch.GetElapsedTime(started) < ObservationTimeout)
        {
            var received = Received.Create();
            if (source.Recv(received, RecvFlags.DontWait))
                return received;
            received.Dispose();
            await Task.Delay(10);
        }
        throw new TimeoutException("The raw peer did not receive the route handshake record.");
    }
}
