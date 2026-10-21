using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
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
