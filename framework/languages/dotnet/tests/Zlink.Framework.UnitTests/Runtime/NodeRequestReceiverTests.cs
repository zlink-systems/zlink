using System.Text.Json;
using System.Text.Json.Serialization;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class NodeRequestReceiverTests
{
    [Theory]
    [InlineData("normal", false)]
    [InlineData("normal", true)]
    [InlineData("cancel", false)]
    [InlineData("cancel", true)]
    [InlineData("timeout", false)]
    [InlineData("timeout", true)]
    [InlineData("shutdown", false)]
    [InlineData("shutdown", true)]
    public async Task Final_receiver_converts_once_even_after_original_terminal(
        string terminal,
        bool throwing
    )
    {
        ReplyConverter.Throw = throwing;
        ReplyConverter.Reads = 0;
        using var context = Systems.Zlink.Zlink.CreateContext();
        var clock = new ReceiverClock();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "receiver-terminal",
            deadlineTimeProvider: clock
        );
        using var cancellation = new CancellationTokenSource();
        var physical = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task? observation = null;
        Func<ZLinkManagedMeshNode.ManagedRequestCompletion, Reply> decode = completion =>
        {
            if (completion.Result != RequestResult.Ok)
                throw new ZlinkRequestException(
                    (ZlinkRequestException.ErrorCode)(int)completion.Result
                );
            return ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<Reply>(
                ZLinkManagedMeshNode.DecodeDirectApplicationReply(1, completion.Parts),
                "empty reply",
                "failed reply",
                null,
                validateFlow: false
            );
        };
        Func<ZLinkManagedMeshNode.PendingOperation<Reply>, Task<Reply>, Task> produce = (
            operation,
            _
        ) =>
        {
            observation = node.ObserveNativeDeadlineReplyAsync(operation, physical.Task, decode);
            started.SetResult();
            return observation;
        };
        var caller = ObserveCallerAsync(
            node.RunDeadlineRequestAsync(
                TimeSpan.FromSeconds(1),
                cancellation.Token,
                decode,
                produce,
                kind: MeshOperationKind.NodeRequest
            )
        );
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(caller.IsCompleted);
        Exception? originalFailure = null;
        if (terminal != "normal")
        {
            if (terminal == "cancel")
                cancellation.Cancel();
            else if (terminal == "timeout")
            {
                clock.Timestamp += TimeSpan.FromSeconds(2).Ticks;
                node.ExpireOperations();
            }
            else
                node.CloseSubmissions();
            originalFailure = await Record.ExceptionAsync(() =>
                caller.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.NotNull(originalFailure);
            if (terminal == "cancel")
                Assert.Equal(
                    cancellation.Token,
                    Assert
                        .IsAssignableFrom<OperationCanceledException>(originalFailure)
                        .CancellationToken
                );
            else
                Assert.Equal(
                    terminal == "timeout"
                        ? ZlinkRequestException.ErrorCode.TimedOut
                        : ZlinkRequestException.ErrorCode.Terminated,
                    Assert.IsAssignableFrom<ZlinkRequestException>(originalFailure).Result
                );
            await node.Completions.CompletionDrained.WaitAsync(TimeSpan.FromSeconds(5));
        }
        var application = ZLinkClientCallCodec.EncodeEnvelopeParts(
            ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Request,
                "receiver-terminal",
                "Reply"
            ) with
            {
                Kind = ZLinkMessageKind.Response,
            },
            new Reply("reply"),
            null
        );
        Message[] parts =
        [
            Message.From(ZLinkServiceWireCodec.EncodeReply(1, (int)RequestResult.Ok, 0)),
            ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(application),
        ];
        ZLinkMessageParts.DisposeAll(application);
        physical.SetResult(parts);
        await observation!.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1, ReplyConverter.Reads);
        Assert.All(parts, part => Assert.Throws<ObjectDisposedException>(() => _ = part.Size));
        if (terminal == "normal")
        {
            if (throwing)
            {
                var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => caller);
                Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, error.Kind);
                Assert.Same(ReplyConverter.OriginalFailure, error.InnerException);
            }
            else
                Assert.Equal("reply", (await caller).Value);
        }
        else
            Assert.Same(originalFailure, await Record.ExceptionAsync(() => caller));
        Assert.Equal(0, node.PendingOperationCount);
    }

    // The public lifetime awaits the selected terminal and preserves its exception.
    private static async Task<Reply> ObserveCallerAsync(Task<Reply> pending) =>
        await pending.ConfigureAwait(false);

    [JsonConverter(typeof(ReplyConverter))]
    private sealed record Reply(string Value);

    private sealed class ReplyConverter : JsonConverter<Reply>
    {
        internal static int Reads;
        internal static bool Throw;
        internal static readonly JsonException OriginalFailure = new("receiver codec failed");

        public override Reply Read(
            ref Utf8JsonReader reader,
            Type type,
            JsonSerializerOptions options
        )
        {
            Interlocked.Increment(ref Reads);
            using var document = JsonDocument.ParseValue(ref reader);
            if (Throw)
                throw OriginalFailure;
            return new Reply(document.RootElement.GetProperty("Value").GetString()!);
        }

        public override void Write(
            Utf8JsonWriter writer,
            Reply value,
            JsonSerializerOptions options
        )
        {
            writer.WriteStartObject();
            writer.WriteString("Value", value.Value);
            writer.WriteEndObject();
        }
    }

    private sealed class ReceiverClock : TimeProvider
    {
        internal long Timestamp;
        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        public override long GetTimestamp() => Timestamp;
    }

    [Theory]
    [InlineData("parts")]
    [InlineData("completion")]
    [InlineData("received")]
    public async Task Terminal_loser_releases_physical_parts_in_every_representation(
        string representation
    )
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "receiver-loser");
        for (var iteration = 0; iteration < 100; iteration++)
        {
            using var part = Message.From("late-reply");
            IReadOnlyList<Message> parts = [part];
            if (representation == "parts")
                await Lose(static reply => reply.Parts);
            else if (representation == "completion")
                await Lose(static reply => reply);
            else
                await Lose(static reply => new ZLinkBackendRouteReceived(
                    reply.Parts,
                    null,
                    null,
                    null,
                    null
                ));
            Assert.Throws<ObjectDisposedException>(() => _ = part.Size);

            async Task Lose<TReply>(
                Func<ZLinkManagedMeshNode.ManagedRequestCompletion, TReply> decode
            )
            {
                var operation = new ZLinkManagedMeshNode.PendingOperation<TReply>(
                    new MeshOperationId(1, (ulong)iteration + 1),
                    MeshOperationKind.NodeRequest,
                    default,
                    decode
                );
                // The correlation is already retired: the physical receiver must still
                // convert once and reclaim the input, including a retained route record.
                await node.ObserveNativeDeadlineReplyAsync(
                    operation,
                    Task.FromResult(parts),
                    decode
                );
                Assert.False(operation.Completion.IsCompleted);
            }
        }
    }
}
