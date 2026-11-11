namespace Zlink.Framework.UnitTests;

public sealed class PausedSerialExecutionTests
{
    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task RestorePrependsAfterPredecessorRemoval(bool shared, bool removeHead)
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        var status = ZLinkObjectExecutionStatus.Paused;
        var selectedAtInvoke = false;
        var restored = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new List<int>();
        await using var queue = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            applicationStartAllowed: item =>
                item.ExecutionStatus != ZLinkObjectExecutionStatus.Paused,
            sharedGate: shared,
            executionAllowed: () => status
        );
        queue.GateOperation = operation =>
        {
            if (operation == ZLinkSerialGateOperation.PausedRestored)
                restored.TrySetResult();
        };
        ZLinkSerialWorkItem Post(int value)
        {
            queue.TryPostApplicationWithAdmission(
                _ =>
                {
                    order.Add(value);
                    return ValueTask.CompletedTask;
                },
                out var item
            );
            return item;
        }
        var head = Post(1);
        var previous = Post(2);
        var selected = Post(3);
        var tail = Post(4);
        ZLinkSerialWorkItem? appended = null;
        queue.ConsumerSelected = () =>
        {
            if (selected.Next is not null)
                return;
            queue.ConsumerSelected = null;
            status = ZLinkObjectExecutionStatus.Paused;
            selectedAtInvoke = true;
        };
        // Only the selected item may pass the paused prefix at this boundary.
        selected.ExecutionAllowed = () =>
        {
            if (!selectedAtInvoke)
                return ZLinkObjectExecutionStatus.Allowed;
            selectedAtInvoke = false;
            appended = Post(5);
            queue._applicationQueue.Remove(previous, head);
            previous.ReleaseForRelocation(exception =>
                throw new InvalidOperationException("Withdrawal failed.", exception)
            );
            if (removeHead)
            {
                queue._applicationQueue.Remove(head, null);
                head.ReleaseForRelocation(exception =>
                    throw new InvalidOperationException("Withdrawal failed.", exception)
                );
            }
            selected.ExecutionAllowed = () => status;
            return ZLinkObjectExecutionStatus.Paused;
        };
        try
        {
            queue.NotifyReadiness();
            await restored.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await queue.RunLifecycleAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.Equal(
                removeHead
                    ? new[] { selected, tail, appended }
                    : new[] { selected, head, tail, appended },
                queue._applicationQueue.ToArray()
            );
            Assert.Equal(removeHead ? 3 : 4, queue._applicationQueue.Count);
            Assert.Equal(removeHead ? 3 : 4, queue.ApplicationPendingCount);
            Assert.False(selected.Completion.IsCompleted);
            status = ZLinkObjectExecutionStatus.Allowed;
            queue.NotifyReadiness();
            await Task.WhenAll(
                    head.Completion,
                    selected.Completion,
                    tail.Completion,
                    appended!.Completion
                )
                .WaitAsync(TimeSpan.FromSeconds(5));
            await queue.ApplicationDrained.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(removeHead ? new[] { 3, 4, 5 } : new[] { 3, 1, 4, 5 }, order);
        }
        finally
        {
            queue.ConsumerSelected = null;
            queue.GateOperation = null;
            if (!selected.Completion.IsCompleted)
            {
                queue._applicationQueue.Clear();
                foreach (var pending in new[] { head, selected, tail, appended! })
                {
                    pending.Next = null;
                    if (!pending.Completion.IsCompleted)
                        queue._applicationQueue.Enqueue(pending);
                }
            }
            status = ZLinkObjectExecutionStatus.Allowed;
            queue.NotifyReadiness();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SharedRestoreSerializesWithProducerAdmission(bool member)
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var status = ZLinkObjectExecutionStatus.Paused;
        var order = new List<int>();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var owner = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        await using var queue = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            applicationStartAllowed: item =>
                item.ExecutionStatus != ZLinkObjectExecutionStatus.Paused,
            sharedGate: !member,
            sharedOwner: member ? owner : null,
            executionAllowed: () => status
        );
        var gate = typeof(ZLinkSerialExecutionQueue)
            .GetField(
                "_admissionGate",
                System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic
            )!
            .GetValue(queue)!;
        var restored = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var publish = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        queue.TryPostApplicationWithAdmission(
            _ =>
            {
                order.Add(0);
                return ValueTask.CompletedTask;
            },
            out var selected
        );
        queue.ConsumerSelected = () =>
        {
            queue.ConsumerSelected = null;
            status = ZLinkObjectExecutionStatus.Paused;
            publish.TrySetResult();
        };
        queue.GateOperation = operation =>
        {
            if (operation == ZLinkSerialGateOperation.PausedRestored)
                restored.TrySetResult(Monitor.IsEntered(gate));
        };
        var producer = Task.Run(async () =>
        {
            await publish.Task;
            var items = new List<ZLinkSerialWorkItem>();
            for (var i = 1; i <= 32; i++)
            {
                var value = i;
                Assert.Equal(
                    ZLinkSerialPostAdmission.Accepted,
                    queue.TryPostApplicationWithAdmission(
                        _ =>
                        {
                            order.Add(value);
                            return ValueTask.CompletedTask;
                        },
                        out var item
                    )
                );
                items.Add(item);
            }
            return items;
        });
        try
        {
            status = ZLinkObjectExecutionStatus.Allowed;
            queue.NotifyReadiness();
            var lockHeld = await restored.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var items = await producer.WaitAsync(TimeSpan.FromSeconds(5));
            await queue.RunLifecycleAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.True(lockHeld, "Paused restoration must hold the mailbox admission gate.");
            Assert.Equal(33, queue.ApplicationPendingCount);
            Assert.Equal(33, queue._applicationQueue.Count);
            Assert.Equal(new[] { selected }.Concat(items), queue._applicationQueue.ToArray());
            Assert.Empty(order);
            status = ZLinkObjectExecutionStatus.Allowed;
            queue.NotifyReadiness();
            await Task.WhenAll(items.Select(item => item.Completion).Append(selected.Completion))
                .WaitAsync(TimeSpan.FromSeconds(5));
            await queue.ApplicationDrained.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(Enumerable.Range(0, 33), order);
        }
        finally
        {
            queue.ConsumerSelected = null;
            queue.GateOperation = null;
            status = ZLinkObjectExecutionStatus.Allowed;
            queue.NotifyReadiness();
        }
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    public async Task PauseAfterSelectionRestoresHeadBeforePreviouslyPausedItem(int placement)
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var owner = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        var status = ZLinkObjectExecutionStatus.Paused;
        var headReady = false;
        var order = new List<int>();
        await using var queue = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            applicationStartAllowed: item =>
                item.ExecutionStatus != ZLinkObjectExecutionStatus.Paused,
            sharedGate: placement == 1,
            sharedOwner: placement == 2 ? owner : null,
            executionAllowed: () => status
        );
        var paused = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        try
        {
            queue.TryPostApplicationWithAdmission(
                _ =>
                {
                    order.Add(1);
                    return ValueTask.CompletedTask;
                },
                0,
                0,
                false,
                out var head,
                ready: () => headReady
            );
            queue.TryPostApplicationWithAdmission(
                _ =>
                {
                    order.Add(2);
                    return ValueTask.CompletedTask;
                },
                0,
                0,
                false,
                out var selected
            );
            selected.ExecutionAllowed = () => ZLinkObjectExecutionStatus.Allowed;
            queue.ConsumerSelected = () =>
            {
                if (selected.Next is not null)
                    return;
                queue.ConsumerSelected = null;
                selected.ExecutionAllowed = () => status;
                status = ZLinkObjectExecutionStatus.Paused;
                paused.TrySetResult();
            };
            headReady = true;
            queue.NotifyReadiness();
            await paused.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await queue.RunLifecycleAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.False(head.Completion.IsCompleted);
            Assert.False(selected.Completion.IsCompleted);
            Assert.Empty(order);
            Assert.Equal(2, queue.ApplicationPendingCount);
            Assert.Equal(new[] { selected, head }, queue._applicationQueue.ToArray());
            status = ZLinkObjectExecutionStatus.Allowed;
            queue.NotifyReadiness();
            await Task.WhenAll(head.Completion, selected.Completion)
                .WaitAsync(TimeSpan.FromSeconds(5));
            await queue.ApplicationDrained.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(new[] { 2, 1 }, order);
            Assert.Equal(0, queue.ApplicationPendingCount);
        }
        finally
        {
            queue.ConsumerSelected = null;
            headReady = true;
            status = ZLinkObjectExecutionStatus.Allowed;
            queue.NotifyReadiness();
        }
    }

    [Theory]
    [InlineData(false, false, 0)]
    [InlineData(true, false, 0)]
    [InlineData(false, true, 0)]
    [InlineData(true, true, 0)]
    [InlineData(false, false, 1)]
    [InlineData(true, false, 1)]
    [InlineData(false, false, 2)]
    [InlineData(true, false, 2)]
    public async Task YieldResumeIsHeldAtSelectionAndInvoke(
        bool shared,
        bool pauseAfterSelection,
        int terminal
    )
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var status = ZLinkObjectExecutionStatus.Allowed;
        ZLinkSerialWorkItem? resumeCandidate = null;
        var paused = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            applicationStartAllowed: item =>
            {
                if (!item.ReservationHeld && item.Mailbox is null)
                    resumeCandidate = item;
                return item.ExecutionStatus != ZLinkObjectExecutionStatus.Paused;
            },
            sharedGate: shared,
            executionAllowed: () => status
        );
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        // Inline completion fixes publication before the following lifecycle observation.
        var completeIo = new TaskCompletionSource();
        var resumed = 0;
        var headReady = false;
        try
        {
            var running = queue
                .RunAsync(
                    async ct =>
                    {
                        started.TrySetResult();
                        await ZLinkSerialTurn
                            .Current!.YieldFrameworkCallAsync(
                                _ => new ValueTask(completeIo.Task),
                                ct
                            )
                            .ConfigureAwait(false);
                        resumed++;
                    },
                    CancellationToken.None
                )
                .AsTask();
            await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await queue.RunLifecycleAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            queue.TryPostApplicationWithAdmission(
                _ => ValueTask.CompletedTask,
                0,
                0,
                false,
                out var head,
                ready: () => headReady
            );
            if (pauseAfterSelection)
                queue.ConsumerSelected = () =>
                {
                    if (resumeCandidate is null || resumeCandidate.Next is not null)
                        return;
                    queue.ConsumerSelected = null;
                    status = ZLinkObjectExecutionStatus.Paused;
                    paused.TrySetResult();
                };
            else
                status = ZLinkObjectExecutionStatus.Paused;
            completeIo.TrySetResult();
            if (pauseAfterSelection)
                await paused.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await queue.RunLifecycleAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.False(running.IsCompleted);
            Assert.Equal(0, resumed);
            Assert.Equal(2, queue.ApplicationPendingCount);
            Assert.Same(pauseAfterSelection ? resumeCandidate : head, queue._applicationQueue.Head);
            headReady = true;
            status = terminal switch
            {
                1 => ZLinkObjectExecutionStatus.Stopped,
                2 => ZLinkObjectExecutionStatus.OwnershipLost,
                _ => ZLinkObjectExecutionStatus.Allowed,
            };
            queue.NotifyReadiness();
            if (terminal == 0)
            {
                await running.WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Equal(1, resumed);
            }
            else
            {
                var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    running.WaitAsync(TimeSpan.FromSeconds(5))
                );
                Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, error.Kind);
                Assert.Equal(0, resumed);
            }
            if (terminal == 1)
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                    head.Completion.WaitAsync(TimeSpan.FromSeconds(5))
                );
            else if (terminal == 2)
            {
                var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    head.Completion.WaitAsync(TimeSpan.FromSeconds(5))
                );
                Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
            }
            else
                await head.Completion.WaitAsync(TimeSpan.FromSeconds(5));
            await queue.ApplicationDrained.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(0, queue.ApplicationPendingCount);
        }
        finally
        {
            queue.ConsumerSelected = null;
            status = ZLinkObjectExecutionStatus.Allowed;
            headReady = true;
            completeIo.TrySetResult();
            queue.NotifyReadiness();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task AcceptedClaimIsRestoredBeforeRecoveryWake(bool shared)
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var status = ZLinkObjectExecutionStatus.Allowed;
        var ready = false;
        var releases = 0;
        var calls = 0;
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            applicationStartAllowed: item =>
                ready && item.ExecutionStatus != ZLinkObjectExecutionStatus.Paused,
            sharedGate: shared,
            executionAllowed: () => status
        );
        var paused = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        try
        {
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    ReadOnlyMemory<byte>.Empty,
                    _ =>
                    {
                        calls++;
                        return ValueTask.CompletedTask;
                    },
                    () => releases++,
                    out var item
                )
            );
            queue.ConsumerSelected = () =>
            {
                if (item.Next is not null)
                    return;
                queue.ConsumerSelected = null;
                status = ZLinkObjectExecutionStatus.Paused;
                paused.TrySetResult();
            };
            ready = true;
            queue.NotifyReadiness();
            await paused.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await queue.RunLifecycleAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.False(item.Completion.IsCompleted);
            Assert.Equal(0, calls);
            Assert.Equal(0, releases);
            Assert.Equal(1, queue.ApplicationPendingCount);
            status = ZLinkObjectExecutionStatus.Allowed;
            queue.NotifyReadiness();
            await item.Completion.WaitAsync(TimeSpan.FromSeconds(5));
            await queue.ApplicationDrained.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(1, calls);
            Assert.Equal(0, releases);
            // A leaked accepted-operation claim would prevent this seal from completing.
            await queue
                .SealRelocationAsync(CancellationToken.None)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            queue.ConsumerSelected = null;
            ready = true;
            status = ZLinkObjectExecutionStatus.Allowed;
            queue.NotifyReadiness();
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task ClosedOrSealedOwnerStillRejectsYieldResume(bool shared, bool seal)
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var status = ZLinkObjectExecutionStatus.Allowed;
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            applicationStartAllowed: item =>
                item.ExecutionStatus != ZLinkObjectExecutionStatus.Paused,
            sharedGate: shared,
            executionAllowed: () => status
        );
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var completeIo = new TaskCompletionSource();
        var resumed = false;
        try
        {
            var running = queue
                .RunAsync(
                    async ct =>
                    {
                        started.TrySetResult();
                        await ZLinkSerialTurn
                            .Current!.YieldFrameworkCallAsync(
                                _ => new ValueTask(completeIo.Task),
                                ct
                            )
                            .ConfigureAwait(false);
                        resumed = true;
                    },
                    CancellationToken.None
                )
                .AsTask();
            await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await queue.RunLifecycleAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            if (seal)
            {
                await queue.SealRelocationAsync(CancellationToken.None);
                status = ZLinkObjectExecutionStatus.OwnershipLost;
            }
            else
                queue.Complete();
            completeIo.TrySetResult();
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                running.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, error.Kind);
            Assert.False(resumed);
            await queue.ApplicationDrained.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            status = ZLinkObjectExecutionStatus.Allowed;
            completeIo.TrySetResult();
            queue.NotifyReadiness();
        }
    }
}
