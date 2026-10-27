using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData("channel", false)]
    [InlineData("node", false)]
    [InlineData("spot", false)]
    [InlineData("channel", true)]
    [InlineData("node", true)]
    [InlineData("spot", true)]
    public async Task StoppedRouteChannel_RejectsRetainedSendAndRequestWithShuttingDown(
        string path,
        bool request
    )
    {
        var node = new CapturingSpotNode { SpotSendResult = SubmitResult.Ok };
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeInstanceSpotRoute: true,
            includeEntryChannelMembership: true
        );
        try
        {
            var route = new ZLinkRouteClient(runtime);
            var target = Assert.IsType<ZLinkResolvedSpotHandle>(
                (
                    await runtime.ResolveInstanceSpotAsync(
                        new InstanceSpotIntentAddress(string.Empty, string.Empty, "spot-ready"),
                        CancellationToken.None
                    )
                ).Handle
            );
            var payload = new ProbeRouteMessage("stopped");
            IZLinkSendCall MakeSend() => path switch
            {
                "channel" => route.SendToChannel("entry", payload),
                "node" => route.SendToNode("entry", RoutingId.From("target-node"), payload),
                _ => new ZLinkRouteSpotSendCall<ProbeRouteMessage>(runtime, target, payload),
            };
            var send = MakeSend();
            IZLinkRequestCall call = path switch
            {
                "channel" => route.RequestToChannel("entry", payload),
                "node" => route.RequestToNode("entry", RoutingId.From("target-node"), payload),
                _ => new ZLinkRouteSpotRequestCall<ProbeRouteMessage>(runtime, target, payload),
            };
            // The same route accepts a send before shutdown without a native socket.
            if (path != "channel")
                await MakeSend().Async();
            await runtime.StopAsync(CancellationToken.None);
            var submissions = node.NodeSendAsyncCalls;
            var spotSubmissions = node.SpotSends.Count;
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            {
                if (request)
                    await call.Async<ProbeReply>();
                else
                    await send.Async();
            });
            Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, failure.Kind);
            Assert.Equal(submissions, node.NodeSendAsyncCalls);
            Assert.Equal(spotSubmissions, node.SpotSends.Count);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task UnstartedRouteChannel_StateAccessPreservesInvalidOperation()
    {
        var registration = new ZLinkFrameworkRegistration();
        await using var services = new ServiceCollection().BuildServiceProvider();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(new CapturingSpotNode()),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration
            )
        );
        Assert.Throws<InvalidOperationException>(() => runtime.GetMeshNodeRuntime("entry"));
    }

    public static IEnumerable<object[]> RequestSubmissionFailures()
    {
        foreach (var path in new[] { "node", "channel", "spot" })
        foreach (
            var failure in new[]
            {
                "unexpected-sync",
                "unexpected-async",
                "cancelled",
                "configuration",
                "binding-configuration",
                "argument",
                "state",
                "disposed",
                "framework",
                "submit-not-found",
                "submit-unavailable",
                "terminal-deadline",
                "terminal-protocol",
                "typed-terminal",
            }
        )
            yield return [path, failure];
    }

    [Theory]
    [MemberData(nameof(RequestSubmissionFailures))]
    public async Task RequestSubmissionFailureContract_PreservesClassificationAndCause(
        string path,
        string failureCase
    )
    {
        Exception original = failureCase switch
        {
            "unexpected-sync" or "unexpected-async" => new IOException("submit failed"),
            "cancelled" => new OperationCanceledException("cancelled"),
            "configuration" => new ZLinkConfigurationException("invalid configuration"),
            "binding-configuration" => new ZlinkConfigException(
                ZlinkConfigException.ErrorCode.InvalidState
            ),
            "argument" => new ArgumentException("invalid argument"),
            "state" => new InvalidOperationException("invalid state"),
            "disposed" => new ObjectDisposedException("request handle"),
            "framework" => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TypeMismatch,
                "typed failure"
            ),
            "submit-not-found" => new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotFound),
            "submit-unavailable" => new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.NotConnected
            ),
            "terminal-deadline" => new ZlinkRequestException(
                ZlinkRequestException.ErrorCode.TimedOut
            ),
            "terminal-protocol" => new ZlinkRequestException(
                ZlinkRequestException.ErrorCode.ProtocolError
            ),
            "typed-terminal" => new ZLinkRequestTerminalException(RequestResult.InvalidState, 0),
            _ => throw new ArgumentOutOfRangeException(nameof(failureCase)),
        };
        IReadOnlyList<Message>? submittedParts = null;
        ValueTask<ZLinkBackendRouteReceived> Fail(IReadOnlyList<Message> parts)
        {
            submittedParts = parts;
            if (failureCase == "unexpected-sync")
                throw original;
            return ValueTask.FromException<ZLinkBackendRouteReceived>(original);
        }
        var node = new CapturingSpotNode
        {
            NodeRequestHandler = (parts, _) => Fail(parts),
            ChannelRequestSubmissionHandler = Fail,
            SpotRequestSubmissionHandler = (_, parts) => Fail(parts),
        };
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeSpotRoute: true,
            includeEntryChannelMembership: true
        );
        try
        {
            var request = new ProbeRouteMessage("request");
            var failure = await Record.ExceptionAsync(async () =>
            {
                if (path == "spot")
                    await new ZLinkSpotClient(runtime)
                        .RequestToSpot("spot-ready", request)
                        .Async<ProbeReply>();
                else if (path == "channel")
                    await new ZLinkRouteClient(runtime)
                        .RequestToChannel("entry", request)
                        .Async<ProbeReply>();
                else
                    await new ZLinkRouteClient(runtime)
                        .RequestToNode("entry", RoutingId.From("target-node"), request)
                        .Async<ProbeReply>();
            });
            Assert.NotNull(failure);
            if (
                failureCase
                is "cancelled"
                    or "configuration"
                    or "binding-configuration"
                    or "argument"
                    or "state"
                    or "disposed"
                    or "framework"
            )
                Assert.Same(original, failure);
            else
            {
                var mapped = Assert.IsType<ZLinkFrameworkException>(failure);
                var expected = failureCase switch
                {
                    "submit-not-found" => path == "channel"
                        ? ZLinkFrameworkErrorKind.Unavailable
                        : ZLinkFrameworkErrorKind.NotFound,
                    "submit-unavailable" => ZLinkFrameworkErrorKind.Unavailable,
                    "terminal-deadline" => ZLinkFrameworkErrorKind.DeadlineExceeded,
                    "terminal-protocol" => ZLinkFrameworkErrorKind.ProtocolError,
                    "typed-terminal" => ZLinkFrameworkErrorKind.InvalidOperation,
                    _ => ZLinkFrameworkErrorKind.InternalFailure,
                };
                Assert.Equal(expected, mapped.Kind);
                if (failureCase is "unexpected-sync" or "unexpected-async")
                    Assert.Same(original, mapped.InnerException);
            }
            Assert.NotNull(submittedParts);
            Assert.All(
                submittedParts,
                part => Assert.Throws<ObjectDisposedException>(() => part.AsReadOnlySpan())
            );
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }
}
