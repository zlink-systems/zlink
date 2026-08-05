using System.Collections.Concurrent;
using System.Diagnostics;

namespace Zlink.Framework.UnitTests;

[Collection(DiagnosticsIsolationCollection.Name)]
public sealed class ZLinkAsyncSubmitterTests : IDisposable
{
    public ZLinkAsyncSubmitterTests() =>
        ZLinkTelemetry.SetDiagnosticsLevel(ZLinkDiagnosticsLevel.Normal);

    public void Dispose() =>
        ZLinkTelemetry.SetDiagnosticsLevel(ZLinkDiagnosticsLevel.Off);

    [Fact]
    public void Transport_Byte_Hwm_Does_Not_Define_Operation_Count_Capacity()
    {
        Assert.True(ZLinkAsyncSubmitter.ResolvePendingCapacity() > 1);
    }

    [Fact]
    public async Task Admission_Trace_Records_One_Signal_And_One_Retry_With_Cleanup()
    {
        var events = new ConcurrentQueue<(string Name, int Pending)>();
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = static (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllData,
            ActivityStopped = activity =>
            {
                if (!string.Equals(activity.OperationName, "zlink.submit.admission", StringComparison.Ordinal))
                    return;
                events.Enqueue((
                    Assert.IsType<string>(activity.GetTagItem("zlink.submit.event")),
                    Assert.IsType<int>(activity.GetTagItem("zlink.submit.pending_waiters"))));
            }
        };
        ActivitySource.AddActivityListener(listener);

        Action? ready = null;
        var writable = false;
        await using var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(1),
            CancellationToken.None,
            capacity: 1);

        var pending = submitter.Async(Message.From("payload"), _ => writable);
        Assert.False(pending.IsCompleted);
        writable = true;
        ready?.Invoke();
        await pending;

        var snapshot = events.ToArray();
        Assert.Equal(2, snapshot.Count(item => item.Name == "transport-attempt"));
        Assert.Single(snapshot, item => item.Name == "pending");
        Assert.Single(snapshot, item => item.Name == "send-ready");
        Assert.Single(snapshot, item => item.Name == "retry-attempt");
        Assert.Single(snapshot, item => item.Name == "commit");
        var cleanup = Assert.Single(snapshot, item => item.Name == "cleanup");
        Assert.Equal(0, cleanup.Pending);
    }

    [Fact]
    public async Task Async_DrainsPendingItemFromReadyCallback()
    {
        Action? ready = null;
        var writable = false;
        var submitted = 0;

        await using var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        var task = submitter.Async(
            Message.From("payload"),
            _ =>
            {
                submitted++;
                return writable;
            });

        Assert.False(task.IsCompleted);
        Assert.Equal(1, submitted);

        writable = true;
        ready?.Invoke();
        await task;

        Assert.Equal(2, submitted);
    }

    [Fact]
    public async Task Async_RetriesAfterQueueingToCloseInlineReadyRace()
    {
        Action? ready = null;
        await using var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);
        var submitted = 0;

        await submitter.Async(
            Message.From("payload"),
            _ =>
            {
                submitted++;
                if (submitted == 1) ready?.Invoke();
                return submitted == 2;
            });

        Assert.Equal(2, submitted);
    }

    [Fact]
    public async Task Async_RetriesWithFreshMessageCopies()
    {
        Action? ready = null;
        var submitted = 0;

        await using var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        var task = submitter.Async(
            Message.From("payload"),
            message =>
            {
                submitted++;
                Assert.Equal("payload", message.GetString());
                return submitted == 2;
            });

        Assert.False(task.IsCompleted);
        Assert.Equal(1, submitted);

        ready?.Invoke();
        await task;

        Assert.Equal(2, submitted);
    }

    [Fact]
    public async Task Async_RetainsOriginalWhenRetryableAttemptConsumesMessage()
    {
        Action? ready = null;
        var submitted = 0;

        await using var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        var task = submitter.Async(
            Message.From("payload"),
            message =>
            {
                submitted++;
                Assert.Equal("payload", message.GetString());
                if (submitted < 2)
                {
                    message.Dispose();
                    return false;
                }

                return true;
            });

        Assert.False(task.IsCompleted);
        Assert.Equal(1, submitted);

        ready?.Invoke();
        await task;

        Assert.Equal(2, submitted);
    }

    [Fact]
    public async Task Async_UsesAtMostOneRetryPerReadySignal()
    {
        Action? ready = null;
        var firstWritable = false;
        var secondWritable = false;
        var firstAttempts = 0;
        var secondAttempts = 0;

        await using var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        var first = submitter.Async(
            Message.From("first"),
            _ =>
            {
                firstAttempts++;
                return firstWritable;
            });
        var second = submitter.Async(
            Message.From("second"),
            _ =>
            {
                secondAttempts++;
                return secondWritable;
            });

        Assert.Equal(1, firstAttempts);
        Assert.Equal(1, secondAttempts);

        firstWritable = true;
        ready?.Invoke();
        await first;

        Assert.Equal(2, firstAttempts);
        Assert.Equal(1, secondAttempts);
        Assert.False(second.IsCompleted);

        secondWritable = true;
        ready?.Invoke();
        await second;
        Assert.Equal(2, secondAttempts);
    }

    [Fact]
    public async Task Async_CancellationRemovesANonHeadPendingReservation()
    {
        await using var submitter = new ZLinkAsyncSubmitter(
            _ => { },
            TimeSpan.FromSeconds(1),
            CancellationToken.None,
            capacity: 2);
        using var cancellation = new CancellationTokenSource();

        var first = submitter.Async(Message.From("first"), _ => false);
        var cancelled = submitter.Async(
            Message.From("cancelled"),
            _ => false,
            cancellation.Token);
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => cancelled.AsTask());

        var replacement = submitter.Async(Message.From("replacement"), _ => false);
        Assert.False(replacement.IsCompleted);

        await submitter.DisposeAsync();
        await Assert.ThrowsAsync<ObjectDisposedException>(() => first.AsTask());
        await Assert.ThrowsAsync<ObjectDisposedException>(() => replacement.AsTask());
    }

    [Fact]
    public async Task Async_CancellationPreventsLateAdmissionAfterReadySignal()
    {
        Action? ready = null;
        var attempts = 0;
        var writable = false;
        using var cancellation = new CancellationTokenSource();
        await using var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(1),
            CancellationToken.None,
            capacity: 1);

        var pending = submitter.Async(
            Message.From("payload"),
            _ =>
            {
                attempts++;
                return writable;
            },
            cancellation.Token);

        Assert.Equal(1, attempts);
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending.AsTask());

        writable = true;
        Assert.NotNull(ready);
        ready();

        Assert.Equal(1, attempts);
    }

    [Fact]
    public async Task Async_FailsPendingItemWhenSendTimeoutExpires()
    {
        await using var submitter = new ZLinkAsyncSubmitter(
            _ => { },
            TimeSpan.FromMilliseconds(20),
            CancellationToken.None);

        var task = submitter.Async(
            Message.From("payload"),
            _ => false);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await task.AsTask());
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
    }

    [Fact]
    public async Task Async_TimeoutPreventsLateAdmissionAfterReadySignal()
    {
        Action? ready = null;
        var attempts = 0;
        var writable = false;
        await using var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromMilliseconds(20),
            CancellationToken.None,
            capacity: 1);

        var pending = submitter.Async(
            Message.From("payload"),
            _ =>
            {
                attempts++;
                return writable;
            });

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => pending.AsTask());
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
        writable = true;
        Assert.NotNull(ready);
        ready();

        Assert.Equal(1, attempts);
    }

    [Fact]
    public async Task Async_WaitsUntilSendTimeoutWhenPendingCapacityIsFull()
    {
        await using var submitter = new ZLinkAsyncSubmitter(
            _ => { },
            TimeSpan.FromMilliseconds(100),
            CancellationToken.None,
            1);

        var first = submitter.Async(
            Message.From("first"),
            _ => false);

        Assert.False(first.IsCompleted);

        var second = submitter.Async(
            Message.From("second"),
            _ => false);

        Assert.False(second.IsCompleted);
        var secondError = await Assert.ThrowsAnyAsync<Exception>(() => second.AsTask());
        var firstError = await Assert.ThrowsAnyAsync<Exception>(() => first.AsTask());
        AssertTimeout(firstError);
        AssertTimeout(secondError);

        static void AssertTimeout(Exception error)
        {
            if (error is TimeoutException) return;
            var deadlineError = Assert.IsType<ZLinkFrameworkException>(error);
            Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, deadlineError.Kind);
        }
    }

    [Fact]
    public async Task Async_BoundsPendingCapacityWaitersAndNeverLateAdmitsOverflow()
    {
        Action? ready = null;
        var writable = false;
        var admitted = 0;
        await using var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(2),
            CancellationToken.None,
            capacity: 1);

        bool Submit(Message _)
        {
            if (!writable) return false;
            Interlocked.Increment(ref admitted);
            return true;
        }

        var first = submitter.Async(Message.From("first"), Submit).AsTask();
        var waiter = submitter.Async(Message.From("waiter"), Submit).AsTask();
        Assert.True(SpinWait.SpinUntil(
            () => submitter.PendingAdmissionWaiterCount == 1,
            TimeSpan.FromSeconds(1)));

        var overflow = Enumerable.Range(0, 32)
            .Select(index => submitter.Async(Message.From($"overflow-{index}"), Submit).AsTask())
            .ToArray();
        foreach (var rejected in overflow)
            await Assert.ThrowsAsync<TimeoutException>(() => rejected);

        Assert.Equal(1, submitter.PendingAdmissionWaiterCount);
        writable = true;
        Assert.NotNull(ready);
        ready();
        await first.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.True(SpinWait.SpinUntil(
            () => submitter.PendingAdmissionWaiterCount == 0,
            TimeSpan.FromSeconds(1)));
        ready();
        await waiter.WaitAsync(TimeSpan.FromSeconds(1));

        Assert.Equal(2, admitted);
    }

    [Fact]
    public async Task Async_ShutdownReleasesBoundedPendingCapacityWaiter()
    {
        using var shutdown = new CancellationTokenSource();
        await using var submitter = new ZLinkAsyncSubmitter(
            _ => { },
            TimeSpan.FromSeconds(2),
            shutdown.Token,
            capacity: 1);
        var first = submitter.SubmitAsync([Message.From("first")], _ => false).AsTask();
        var waiter = submitter.SubmitAsync([Message.From("waiter")], _ => false).AsTask();
        Assert.True(SpinWait.SpinUntil(
            () => submitter.PendingAdmissionWaiterCount == 1,
            TimeSpan.FromSeconds(1)));

        shutdown.Cancel();

        Assert.Equal(ZLinkOneWaySubmitStatus.Shutdown, (await first).Status);
        Assert.Equal(ZLinkOneWaySubmitStatus.Shutdown, (await waiter).Status);
        Assert.True(SpinWait.SpinUntil(
            () => submitter.PendingAdmissionWaiterCount == 0,
            TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public async Task Async_Throws_NonRetryable_Submit_Failure_On_The_Caller()
    {
        await using var submitter = new ZLinkAsyncSubmitter(
            _ => { },
            TimeSpan.FromSeconds(1),
            CancellationToken.None,
            failFastNotConnected: static () => true);

        var exception = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            submitter.Async(
                Message.From("payload"),
                _ => throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected))
                .AsTask());

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, exception.Kind);
    }

    [Fact]
    public async Task DisposeAsync_FailsPendingItems()
    {
        var submitter = new ZLinkAsyncSubmitter(
            _ => { },
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        var pending = submitter.Async(
            Message.From("payload"),
            _ => false);

        await submitter.DisposeAsync();

        await Assert.ThrowsAsync<ObjectDisposedException>(async () => await pending.AsTask());
    }

    [Fact]
    public async Task DisposeAsync_Twice_RacingReady_CleansPendingResourcesOnce()
    {
        using var operation = new Activity("submit-dispose-race").Start();
        var operationId = Assert.IsType<string>(operation.Id);
        var events = new ConcurrentQueue<(string Name, int Pending, int Reservations, int Callbacks)>();
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = static (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllData,
            ActivityStopped = activity =>
            {
                if (!string.Equals(
                        activity.GetTagItem("zlink.submit.operation_id") as string,
                        operationId,
                        StringComparison.Ordinal))
                    return;
                events.Enqueue((
                    Assert.IsType<string>(activity.GetTagItem("zlink.submit.event")),
                    Assert.IsType<int>(activity.GetTagItem("zlink.submit.pending_waiters")),
                    Assert.IsType<int>(activity.GetTagItem("zlink.submit.reservations")),
                    Assert.IsType<int>(activity.GetTagItem("zlink.submit.callbacks"))));
            }
        };
        ActivitySource.AddActivityListener(listener);

        Action? ready = null;
        var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(1),
            CancellationToken.None,
            capacity: 1);
        var pending = submitter.Async(Message.From("payload"), _ => false);

        var firstDispose = submitter.DisposeAsync().AsTask();
        var secondDispose = submitter.DisposeAsync().AsTask();
        var signal = Task.Run(Assert.IsType<Action>(ready));

        Assert.Same(firstDispose, secondDispose);
        await Task.WhenAll(firstDispose, secondDispose, signal);
        await Assert.ThrowsAsync<ObjectDisposedException>(() => pending.AsTask());

        var snapshot = events.ToArray();
        Assert.Single(snapshot, item => item.Name == "pending");
        var cleanup = Assert.Single(snapshot, item => item.Name == "cleanup");
        Assert.Equal(0, cleanup.Pending);
        Assert.Equal(0, cleanup.Reservations);
        Assert.Equal(0, cleanup.Callbacks);
        Assert.Empty(snapshot.Where(item => item.Name == "commit"));
        Assert.InRange(snapshot.Count(item => item.Name == "transport-attempt"), 1, 2);
        Assert.Equal(
            snapshot.Count(item => item.Name == "send-ready"),
            snapshot.Count(item => item.Name == "retry-attempt"));
    }

    [Fact]
    public async Task DisposeAsync_WaitsForBlockedImmediateNativeSubmit_AndIsShared()
    {
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var submitter = new ZLinkAsyncSubmitter(
            _ => { },
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        var submit = Task.Run(async () =>
            await submitter.Async(
                Message.From("payload"),
                message =>
                {
                    Assert.Equal("payload", message.GetString());
                    entered.TrySetResult();
                    release.Task.GetAwaiter().GetResult();
                    Assert.Equal("payload", message.GetString());
                    return true;
                }));

        await entered.Task;
        var firstDispose = submitter.DisposeAsync().AsTask();
        var secondDispose = submitter.DisposeAsync().AsTask();

        Assert.Same(firstDispose, secondDispose);
        Assert.False(firstDispose.IsCompleted);

        release.TrySetResult();
        await submit;
        await firstDispose;
    }

    [Fact]
    public async Task DisposeAsync_WaitsForBlockedDrainNativeSubmit_BeforeDisposingQueuedParts()
    {
        Action? ready = null;
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var attempts = 0;
        var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        var pending = submitter.Async(
            Message.From("payload"),
            message =>
            {
                Assert.Equal("payload", message.GetString());
                if (Interlocked.Increment(ref attempts) < 2) return false;

                entered.TrySetResult();
                release.Task.GetAwaiter().GetResult();
                Assert.Equal("payload", message.GetString());
                return false;
            });

        Assert.NotNull(ready);
        var drain = Task.Run(ready);
        await entered.Task;

        var dispose = submitter.DisposeAsync().AsTask();
        Assert.False(dispose.IsCompleted);

        release.TrySetResult();
        await drain;
        await dispose;
        await Assert.ThrowsAsync<ObjectDisposedException>(() => pending.AsTask());
        Assert.Equal(2, attempts);
    }

    [Fact]
    public async Task SubmitRequestAsync_ImmediateAcceptedRaw_CancellationEndsWaitAndDisposesLateReply()
    {
        await using var submitter = CreateSubmitter(out _);
        using var cancellation = new CancellationTokenSource();
        Action<IReadOnlyList<Message>>? complete = null;

        var request = submitter.SubmitRequestAsync<IReadOnlyList<Message>>(
            Message.From("request"),
            (_, onResult, _) =>
            {
                complete = onResult;
                return true;
            },
            cancellation.Token,
            ZLinkMessageParts.DisposeAll);

        cancellation.Cancel();
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => request.AsTask());
        Assert.Equal(cancellation.Token, error.CancellationToken);

        using var lateReply = Message.From("late");
        Assert.NotNull(complete);
        complete([lateReply]);
        Assert.Throws<ObjectDisposedException>(() => lateReply.AsReadOnlySpan());
    }

    [Fact]
    public async Task SubmitRequestAsync_QueuedThenAcceptedRaw_CancellationEndsWaitAndDisposesLateReply()
    {
        await using var submitter = CreateSubmitter(out var signalReady);
        using var cancellation = new CancellationTokenSource();
        Action<IReadOnlyList<Message>>? complete = null;
        var writable = false;

        var request = submitter.SubmitRequestAsync<IReadOnlyList<Message>>(
            Message.From("request"),
            (_, onResult, _) =>
            {
                if (!writable) return false;
                complete = onResult;
                return true;
            },
            cancellation.Token,
            ZLinkMessageParts.DisposeAll);

        Assert.False(request.IsCompleted);
        writable = true;
        signalReady();
        Assert.NotNull(complete);

        cancellation.Cancel();
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => request.AsTask());
        Assert.Equal(cancellation.Token, error.CancellationToken);

        using var lateReply = Message.From("late");
        complete([lateReply]);
        Assert.Throws<ObjectDisposedException>(() => lateReply.AsReadOnlySpan());
    }

    [Fact]
    public async Task SubmitRequestAsync_ImmediateAcceptedEnvelope_CancellationDisposesLateNativeReply()
    {
        await using var submitter = CreateSubmitter(out _);
        using var cancellation = new CancellationTokenSource();
        Action<RequestResult, IReadOnlyList<Message>>? nativeComplete = null;

        var request = submitter.SubmitRequestAsync<string>(
            Message.From("request"),
            (_, complete, fail) =>
            {
                nativeComplete = (result, reply) => ZLinkEnvelopeReplyCompletion.Complete(
                    result, reply, complete, fail, "test request");
                return true;
            },
            cancellation.Token);

        cancellation.Cancel();
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => request.AsTask());
        Assert.Equal(cancellation.Token, error.CancellationToken);

        var lateReply = CreateEnvelopeReply("late");
        Assert.NotNull(nativeComplete);
        nativeComplete(RequestResult.Ok, lateReply);
        Assert.All(lateReply, part =>
            Assert.Throws<ObjectDisposedException>(() => part.AsReadOnlySpan()));
    }

    [Fact]
    public async Task SubmitRequestAsync_QueuedThenAcceptedEnvelope_CancellationDisposesLateNativeReply()
    {
        await using var submitter = CreateSubmitter(out var signalReady);
        using var cancellation = new CancellationTokenSource();
        Action<RequestResult, IReadOnlyList<Message>>? nativeComplete = null;
        var writable = false;

        var request = submitter.SubmitRequestAsync<string>(
            Message.From("request"),
            (_, complete, fail) =>
            {
                if (!writable) return false;
                nativeComplete = (result, reply) => ZLinkEnvelopeReplyCompletion.Complete(
                    result, reply, complete, fail, "test request");
                return true;
            },
            cancellation.Token);

        Assert.False(request.IsCompleted);
        writable = true;
        signalReady();
        Assert.NotNull(nativeComplete);

        cancellation.Cancel();
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => request.AsTask());
        Assert.Equal(cancellation.Token, error.CancellationToken);

        var lateReply = CreateEnvelopeReply("late");
        nativeComplete(RequestResult.Ok, lateReply);
        Assert.All(lateReply, part =>
            Assert.Throws<ObjectDisposedException>(() => part.AsReadOnlySpan()));
    }

    [Fact]
    public async Task SubmitRequestAsync_OperationTimeoutCoversRouteAdmissionBeyondSocketDefault()
    {
        await using var submitter = new ZLinkAsyncSubmitter(
            _ => { },
            TimeSpan.FromMilliseconds(50),
            CancellationToken.None);
        Action<string>? complete = null;
        var writable = false;

        var request = submitter.SubmitRequestAsync<string>(
            Message.From("request"),
            (_, onResult, _) =>
            {
                if (!writable)
                    throw new ZlinkSubmitException(
                        ZlinkSubmitException.ErrorCode.NotConnected);
                complete = onResult;
                return true;
            },
            operationTimeout: TimeSpan.FromSeconds(1));

        await Task.Delay(TimeSpan.FromMilliseconds(100));
        writable = true;
        Assert.True(SpinWait.SpinUntil(
            () => Volatile.Read(ref complete) is not null,
            TimeSpan.FromSeconds(1)));
        complete!("reply");

        Assert.Equal("reply", await request);
    }

    [Fact]
    public async Task SubmitRequestAsync_LinkedTimeoutCancellationPreservesTokenAndDisposesLateReply()
    {
        await using var submitter = CreateSubmitter(out _);
        using var callerCancellation = new CancellationTokenSource();
        using var timeout = new CancellationTokenSource();
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            callerCancellation.Token,
            timeout.Token);
        Action<IReadOnlyList<Message>>? complete = null;

        var request = submitter.SubmitRequestAsync<IReadOnlyList<Message>>(
            Message.From("request"),
            (_, onResult, _) =>
            {
                complete = onResult;
                return true;
            },
            linked.Token,
            ZLinkMessageParts.DisposeAll);

        timeout.Cancel();
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => request.AsTask());
        Assert.Equal(linked.Token, error.CancellationToken);

        using var lateReply = Message.From("late");
        Assert.NotNull(complete);
        complete([lateReply]);
        Assert.Throws<ObjectDisposedException>(() => lateReply.AsReadOnlySpan());
    }

    [Fact]
    public async Task SubmitRequestAsync_NormalWinnerTransfersRawReplyAndDisposesDuplicate()
    {
        await using var submitter = CreateSubmitter(out _);
        using var cancellation = new CancellationTokenSource();
        Action<IReadOnlyList<Message>>? complete = null;

        var request = submitter.SubmitRequestAsync<IReadOnlyList<Message>>(
            Message.From("request"),
            (_, onResult, _) =>
            {
                complete = onResult;
                return true;
            },
            cancellation.Token,
            ZLinkMessageParts.DisposeAll);

        using var winner = Message.From("winner");
        Assert.NotNull(complete);
        complete([winner]);
        var result = await request;
        cancellation.Cancel();

        Assert.Same(winner, Assert.Single(result));
        Assert.Equal("winner", winner.GetString());

        using var duplicate = Message.From("duplicate");
        complete([duplicate]);
        Assert.Throws<ObjectDisposedException>(() => duplicate.AsReadOnlySpan());
    }

    private static ZLinkAsyncSubmitter CreateSubmitter(out Action signalReady)
    {
        Action? ready = null;
        var submitter = new ZLinkAsyncSubmitter(
            handler => ready = handler,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);
        signalReady = () =>
        {
            Assert.NotNull(ready);
            ready();
        };
        return submitter;
    }

    private static IReadOnlyList<Message> CreateEnvelopeReply(string body)
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Response,
            "test",
            "Reply",
            ZLinkEnvelopeCodec.DefaultContentType,
            "correlation",
            null,
            null,
            null,
            null);
        return ZLinkEnvelopeCodec.EncodeParts(header, body, typeof(string), null);
    }
}
