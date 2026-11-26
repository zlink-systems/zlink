using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    public sealed class SpotCreationShutdownTests
    {
        private static readonly TimeSpan TestTimeout = TimeSpan.FromSeconds(10);

        [Fact]
        public async Task StopAsync_ClosesPublishedSpotWhenItsPreparationEndsDuringShutdown()
        {
            var preparation = new BlockingSpotCreateProbe();
            var closeClaimed = new TaskCompletionSource<ZLinkSpotActivation>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var submitClose = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var dispatchAttached = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var queueClosed = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var drainQueue = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var native = new CapturingSpot
            {
                DispatchAttached = () =>
                {
                    preparation.Started.Task.WaitAsync(TestTimeout).GetAwaiter().GetResult();
                    dispatchAttached.TrySetResult();
                },
            };
            var node = new CapturingSpotNode { CreatedSpotFactory = () => native };
            var (runtime, _) = await CreateStartedRuntimeAsync(
                node,
                userSpotType: typeof(GatedPreparationSpot),
                blockingCreateProbe: preparation,
                messageFlowMode: ZLinkDiagnosticsLevel.Normal
            );
            var catalog = runtime.GetSpotNodeRuntime("entry").Catalog;
            catalog.BeforeShutdownCloseForTest = activation =>
            {
                closeClaimed.TrySetResult(activation);
                return new ValueTask(submitClose.Task);
            };

            Task? stop = null;
            try
            {
                // Start at the catalog so the test can observe the creation owner's
                // terminal after the runtime has sealed caller admission.
                var create = catalog
                    .CreateAsync(
                        typeof(GatedPreparationSpot),
                        ZLinkMessage.Empty,
                        CancellationToken.None
                    )
                    .AsTask();
                await dispatchAttached.Task.WaitAsync(TestTimeout);
                Assert.Equal(1, catalog.ActiveSpotCount);

                stop = runtime.StopAsync(CancellationToken.None).AsTask();
                var activation = await closeClaimed.Task.WaitAsync(TestTimeout);
                var spot = Assert.IsType<GatedPreparationSpot>(activation.Spot);
                Assert.True(activation.StopToken.IsCancellationRequested);

                // If creation rolls back, hold the queue drain after admission
                // closes and keep the executor's state lane alive for the close submission.
                var gate = activation._serial.ActorIngressOwner!;
                gate.GateOperation = operation =>
                {
                    if (
                        operation != ZLinkSerialGateOperation.Release
                        || !activation.IsDisposed
                        || gate.TryPostNext(static _ => ValueTask.CompletedTask, out _)
                    )
                        return;
                    gate.GateOperation = null;
                    queueClosed.TrySetResult();
                    drainQueue.Task.WaitAsync(TestTimeout).GetAwaiter().GetResult();
                };
                preparation.Release.TrySetResult();
                var firstTerminal = await Task.WhenAny(create, queueClosed.Task)
                    .WaitAsync(TestTimeout);
                gate.GateOperation = null;
                submitClose.TrySetResult();
                if (firstTerminal == queueClosed.Task)
                {
                    using var deadline = new CancellationTokenSource(TestTimeout);
                    while (await catalog.CloseTransactionCountAsync() != 0)
                        await Task.Delay(1, deadline.Token);
                }
                drainQueue.TrySetResult();

                await stop.WaitAsync(TestTimeout);
                Assert.Equal(ZLinkSpotCreateState.Created, (await create).State);
                Assert.Equal(
                    ZLinkSpotCloseReason.HostShutdown,
                    await spot.Closed.Task.WaitAsync(TestTimeout)
                );
                Assert.Equal(1, native.DisposeCount);
            }
            finally
            {
                preparation.Release.TrySetResult();
                submitClose.TrySetResult();
                drainQueue.TrySetResult();
                await (stop ?? runtime.StopAsync(CancellationToken.None).AsTask()).WaitAsync(
                    TestTimeout
                );
            }
        }

        private sealed class GatedPreparationSpot(
            IZLinkSpotContext context,
            BlockingSpotCreateProbe preparation
        ) : IZLinkSpot
        {
            public IZLinkSpotContext Context { get; } = context;

            internal TaskCompletionSource<ZLinkSpotCloseReason> Closed { get; } =
                new(TaskCreationOptions.RunContinuationsAsynchronously);

            public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
                ZLinkMessage request,
                CancellationToken cancellationToken
            )
            {
                var gate = ZLinkSerialTurn.Current!.ExecutionGate!;
                gate.GateOperation = operation =>
                {
                    if (operation != ZLinkSerialGateOperation.Release)
                        return;
                    gate.GateOperation = null;
                    preparation.Started.TrySetResult();
                    preparation.Release.Task.WaitAsync(TestTimeout).GetAwaiter().GetResult();
                };
                return new(ZLinkSpotCreateResponse.Accept());
            }

            public ValueTask OnClosingAsync(
                ZLinkSpotClosingContext closing,
                CancellationToken cleanupCancellationToken
            )
            {
                Closed.SetResult(closing.Reason);
                return ValueTask.CompletedTask;
            }
        }
    }
}
