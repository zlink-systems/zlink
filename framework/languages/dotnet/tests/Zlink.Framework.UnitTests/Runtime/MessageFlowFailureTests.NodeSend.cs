using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class MessageFlowFailureTests
{
    [Theory]
    [InlineData(null, "decode_error", "value")]
    [InlineData("valid", "handler_exception", "node handler failure")]
    public async Task Node_send_reports_the_failure_once(
        string? value,
        string reason,
        string errorMessage
    )
    {
        const string meshName = "node-send-failure";
        var targetRid = RoutingId.From(Guid.NewGuid().ToString("N"));
        using var flow = new FlowObservation("message_kind", "send", nameof(NodeSendPacket));
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var targetBuilder = Host.CreateApplicationBuilder();
        targetBuilder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options
                .AddRouteMesh(meshName)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingId(targetRid)
                .AddRouteSendHandler<NodeSendFailureHandler, NodeSendPacket>();
        });
        using var target = targetBuilder.Build();
        await target.StartAsync(cancellation.Token);
        var endpoint = Assert.IsType<string>(
            target
                .Services.GetRequiredService<ZLinkFrameworkRuntime>()
                .GetMeshNodeRuntime(meshName)
                .Node.MeshStatus()
                .LocalEndpoint
        );

        var sourceBuilder = Host.CreateApplicationBuilder();
        sourceBuilder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options
                .AddRouteMesh(meshName)
                .Listen("tcp://127.0.0.1:0")
                .PeerConnections.Connect(targetRid, endpoint);
        });
        using var source = sourceBuilder.Build();
        await source.StartAsync(cancellation.Token);
        try
        {
            var runtime = source.Services.GetRequiredService<IZLinkRouteMeshRuntime>();
            await foreach (var observation in runtime.ObserveAsync(meshName, cancellation.Token))
            {
                if (
                    observation.Status.Peers.Any(peer =>
                        peer.NodeRid == targetRid && peer.State == ZLinkPeerState.Ready
                    )
                )
                    break;
            }

            var client = source.Services.GetRequiredService<IZLinkRouteClient>();
            await client.SendToNode(meshName, targetRid, new NullableNodeSendPacket(value)).Async();
            await flow.Failure.Task.WaitAsync(cancellation.Token);
        }
        finally
        {
            await source.StopAsync();
            await target.StopAsync();
        }

        MessageFlowDeliveryTests.WaitForDelivery();
        var failure = Assert.Single(
            flow.Events,
            activity => activity.OperationName == "zlink.dispatch_error"
        );
        AssertFailure(failure, reason, "drop");
        Assert.Contains(errorMessage, Tag(failure, "error_message"));
        Assert.Equal(
            value is null ? 0 : 1,
            flow.Events.Count(activity => Tag(activity, "phase") == "dispatched")
        );
    }

    [ZLinkPacket(nameof(NodeSendPacket))]
    public sealed record NullableNodeSendPacket(string? Value);

    public sealed record NodeSendPacket(string Value);

    internal sealed class NodeSendFailureHandler : IZLinkRouteSendHandler<NodeSendPacket>
    {
        public ValueTask HandleAsync(
            NodeSendPacket message,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken
        ) => throw new InvalidOperationException("node handler failure");
    }
}
