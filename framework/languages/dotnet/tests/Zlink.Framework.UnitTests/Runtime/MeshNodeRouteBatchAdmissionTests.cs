using System.Collections.Concurrent;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests.Runtime;

// Shutdown drain must keep waiting for every record of a node-route claim batch
// that is still running, and a record of that batch that starts after the drain
// seal must report ShuttingDown.
public sealed partial class EntrySpotActorDispatchTests
{
    private static readonly TimeSpan BatchAdmissionGuard = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task Drain_waits_for_every_running_record_of_the_batch_and_rejects_later_starts()
    {
        var gates = new BatchGates();
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddSingleton(gates).AddTransient<BatchHandler>();
        builder.Services.AddZLinkFramework(_ => { });
        using var host = builder.Build();
        await host.StartAsync();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        var registration = host.Services.GetRequiredService<ZLinkFrameworkRegistration>();
        var spotNode = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "batch",
            RoutingId = RoutingId.From("batch-node"),
        };
        foreach (var channel in new[] { "drain", "other" })
        {
            var membership = new ZLinkMeshChannelMembership { ChannelName = channel };
            membership.RequestHandlers.Add(
                new ZLinkChannelHandlerRegistration(
                    typeof(BatchHandler),
                    typeof(BatchMessage),
                    typeof(BatchReply),
                    nameof(BatchMessage)
                )
            );
            spotNode.ChannelMemberships.Add(membership);
        }
        var runner = new ZLinkRuntimeTaskRunner(
            new ThrowingRuntimeErrorSink(),
            CancellationToken.None
        );
        await using var dispatcher = Assert.IsType<ZLinkMeshNodeRouteDispatcher>(
            ZLinkMeshNodeRouteDispatcher.Create(
                host.Services,
                registration,
                spotNode,
                runtime,
                runner
            )
        );
        var replies = new ConcurrentDictionary<string, TaskCompletionSource<string>>();
        // "first" and "queued" share the drain channel queue, so "queued" starts
        // only after "first" returns. "second" runs on its own channel queue.
        var dispatch = dispatcher
            .DispatchBatchAsync(
                [Record("drain", "first"), Record("drain", "queued"), Record("other", "second")],
                CancellationToken.None
            )
            .AsTask();
        Task? drained = null;
        try
        {
            await gates.Entered("first").WaitAsync(BatchAdmissionGuard);
            await gates.Entered("second").WaitAsync(BatchAdmissionGuard);

            runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            drained = runtime.WaitForAcceptedOperationsForDrainAsync();
            Assert.False(drained.IsCompleted, "Drain ignored two running records of the batch.");

            gates.Release("first");
            Assert.Equal("first", await Reply("first").WaitAsync(BatchAdmissionGuard));
            // The queued record starts after the seal, inside the admitted batch.
            Assert.Equal("shutting_down", await Reply("queued").WaitAsync(BatchAdmissionGuard));
            Assert.False(gates.Entered("queued").IsCompleted);
            Assert.False(drained.IsCompleted, "Drain ignored the record that is still running.");

            gates.Release("second");
            Assert.Equal("second", await Reply("second").WaitAsync(BatchAdmissionGuard));
            await drained.WaitAsync(BatchAdmissionGuard);
            await dispatch.WaitAsync(BatchAdmissionGuard);
        }
        finally
        {
            gates.Release("first");
            gates.Release("second");
            await dispatch.WaitAsync(BatchAdmissionGuard);
            await runner.StopAsync();
            await host.StopAsync();
        }

        Task<string> Reply(string value) =>
            replies
                .GetOrAdd(
                    value,
                    static _ => new(TaskCreationOptions.RunContinuationsAsynchronously)
                )
                .Task;

        ZLinkBackendRouteReceived Record(string channel, string value)
        {
            var header = new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Request,
                channel,
                nameof(BatchMessage),
                ZLinkEnvelopeCodec.DefaultContentType,
                value,
                null,
                null,
                null,
                null
            );
            return new ZLinkBackendRouteReceived(
                ZLinkEnvelopeCodec.EncodeParts(
                    header,
                    new BatchMessage(value),
                    typeof(BatchMessage),
                    null
                ),
                sourceNodeRid: RoutingId.From("source-node"),
                spotId: null,
                requestSeq: 41,
                reply: parts =>
                {
                    var replyHeader = ZLinkEnvelopeCodec.DecodeHeader(parts);
                    var completion = replies.GetOrAdd(
                        value,
                        static _ => new(TaskCreationOptions.RunContinuationsAsynchronously)
                    );
                    if (replyHeader.Kind == ZLinkMessageKind.Error)
                        completion.TrySetResult(replyHeader.ErrorCode ?? "error");
                    else
                        completion.TrySetResult(
                            Assert
                                .IsType<BatchReply>(
                                    ZLinkEnvelopeCodec.DecodeBody(parts, typeof(BatchReply))
                                )
                                .Value
                        );
                    return SubmitResult.Ok;
                },
                channelName: channel
            );
        }
    }

    private sealed record BatchMessage(string Value);

    private sealed record BatchReply(string Value);

    private sealed class BatchGates
    {
        private readonly ConcurrentDictionary<string, TaskCompletionSource> _entered = new();
        private readonly ConcurrentDictionary<string, TaskCompletionSource> _released = new();

        internal Task Entered(string value) => Signal(_entered, value).Task;

        internal void Release(string value) => Signal(_released, value).TrySetResult();

        internal async Task EnterAsync(string value, CancellationToken cancellationToken)
        {
            Signal(_entered, value).TrySetResult();
            await Signal(_released, value).Task.WaitAsync(cancellationToken);
        }

        private static TaskCompletionSource Signal(
            ConcurrentDictionary<string, TaskCompletionSource> signals,
            string value
        ) =>
            signals.GetOrAdd(
                value,
                static _ => new(TaskCreationOptions.RunContinuationsAsynchronously)
            );
    }

    private sealed class BatchHandler(BatchGates gates)
        : IZLinkRequestHandler<BatchMessage, BatchReply>
    {
        public async ValueTask<BatchReply> HandleAsync(
            BatchMessage request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        )
        {
            await gates.EnterAsync(request.Value, cancellationToken);
            return new BatchReply(request.Value);
        }
    }
}
