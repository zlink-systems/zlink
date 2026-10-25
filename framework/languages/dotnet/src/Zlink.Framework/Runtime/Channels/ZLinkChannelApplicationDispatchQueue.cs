using System.Threading.Channels;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Messaging;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelApplicationDispatchQueue<TWork> : IAsyncDisposable
{
    private readonly Channel<DispatchWork<TWork>> _queue = Channel.CreateUnbounded<
        DispatchWork<TWork>
    >(
        new UnboundedChannelOptions
        {
            SingleReader = true,
            SingleWriter = false,
            AllowSynchronousContinuations = false,
        }
    );
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly CancellationTokenSource _stop;
    private readonly string _name;
    private readonly Task _worker;
    private readonly ZLinkProcessExecutionPool.ApplicationSource _applicationSource;
    private readonly Func<TWork, CancellationToken, ValueTask> _dispatch;
    private readonly Action<TWork> _reject;
    private readonly CancellationToken _disposeDeadline;
    private int _stopped;

    internal ZLinkChannelReplyGate ReplyGate { get; } = new();

    internal ZLinkChannelApplicationDispatchQueue(
        string name,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken laneCancellationToken,
        CancellationToken disposeDeadline,
        Func<TWork, CancellationToken, ValueTask> dispatch,
        Action<TWork> reject
    )
    {
        _name = name;
        _disposeDeadline = disposeDeadline;
        _errorSink = errorSink;
        _dispatch = dispatch ?? throw new ArgumentNullException(nameof(dispatch));
        _reject = reject ?? throw new ArgumentNullException(nameof(reject));
        _stop = CancellationTokenSource.CreateLinkedTokenSource(laneCancellationToken);
        // The queue is built inside state-lane turns (fanout subscriber registration). Its worker
        // runs application handlers, so it must not inherit that turn's lane ownership
        // (06-state-ownership-and-lanes §6 type 2).
        _applicationSource = ZLinkProcessExecutionPool.CreateApplicationSource(
            _ => RunTurnAsync(_stop.Token),
            Report
        );
        _worker = _applicationSource.Completion;
    }

    internal ValueTask<bool> PostAsync(TWork payload, CancellationToken cancellationToken)
    {
        var work = new DispatchWork<TWork>(payload);
        if (Volatile.Read(ref _stopped) != 0)
        {
            Reject(work);
            return ValueTask.FromResult(false);
        }

        if (cancellationToken.IsCancellationRequested || !_queue.Writer.TryWrite(work))
        {
            Reject(work);
            return ValueTask.FromResult(false);
        }

        _applicationSource.Signal();
        return ValueTask.FromResult(true);
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _stopped, 1) != 0)
            return;

        _queue.Writer.TryComplete();
        await _stop.CancelAsync().ConfigureAwait(false);

        var disposal = _applicationSource.DisposeAsync().AsTask();

        // Accepted handlers deliver their replies until the host deadline; the reply path
        // closes after them (client-server channel section 6, steps 3 and 4).
        try
        {
            await _worker.WaitAsync(_disposeDeadline).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_disposeDeadline.IsCancellationRequested)
        {
            await ReplyGate.CloseAsync().ConfigureAwait(false);
            // The host deadline expired while a handler ignores cancellation. The worker is
            // observed late, and its stop source is released only after it completes.
            ZLinkUnawaitedSubmit.Observe(
                new ValueTask(CompleteLateStopAsync(disposal)),
                $"channel-application-dispatch-late-stop:{_name}",
                _errorSink
            );
            return;
        }
        catch
        {
            await CompleteLateStopAsync(disposal).ConfigureAwait(false);
            throw;
        }

        await CompleteLateStopAsync(disposal).ConfigureAwait(false);
    }

    private ValueTask RunTurnAsync(CancellationToken cancellationToken)
    {
        for (
            var count = 0;
            count < ZLinkReceiveBatchBudget.MaximumRecords && _queue.Reader.TryRead(out var work);
            count++
        )
        {
            if (cancellationToken.IsCancellationRequested)
            {
                Reject(work);
                continue;
            }
            try
            {
                var result = _dispatch(work.Payload, cancellationToken);
                if (!result.IsCompleted)
                    return CompleteDispatchAsync(result, cancellationToken);
                result.GetAwaiter().GetResult();
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
            catch (Exception exception)
            {
                Report(exception);
            }
        }
        if (_queue.Reader.TryPeek(out _))
            _applicationSource.Signal();
        return ValueTask.CompletedTask;
    }

    private async ValueTask CompleteDispatchAsync(
        ValueTask dispatch,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await dispatch.ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
        catch (Exception exception)
        {
            Report(exception);
        }
        finally
        {
            if (_queue.Reader.TryPeek(out _))
                _applicationSource.Signal();
        }
    }

    private async Task CompleteLateStopAsync(Task disposal)
    {
        var failures = new ZLinkFailureCollector();
        await failures.CaptureAsync(() => new ValueTask(disposal)).ConfigureAwait(false);
        while (_queue.Reader.TryRead(out var pending))
            failures.Capture(() => Reject(pending));
        await failures.CaptureAsync(ReplyGate.CloseAsync).ConfigureAwait(false);
        failures.Capture(_stop.Dispose);
        failures.ThrowIfAny();
    }

    private void Reject(DispatchWork<TWork> work)
    {
        try
        {
            _reject(work.Payload);
        }
        catch (Exception exception)
        {
            Report(exception);
        }
    }

    private void Report(Exception exception)
    {
        try
        {
            _errorSink.ReportRuntimeTaskException(_name, exception);
        }
        catch (Exception reportingFailure)
        {
            throw ReferenceEquals(exception, reportingFailure)
                ? exception
                : new AggregateException(exception, reportingFailure);
        }
    }

    private readonly record struct DispatchWork<TPayload>(TPayload Payload);
}

internal sealed class ZLinkChannelReplyGate
{
    private readonly ZLinkStateLane _lane = new();
    private bool _open = true;
    private int _activeReplies;
    private TaskCompletionSource? _repliesDrained;

    internal ValueTask<bool> TryInvokeAsync(Action reply) =>
        reply is null
            ? ValueTask.FromException<bool>(new ArgumentNullException(nameof(reply)))
            : TryInvokeAsync(static callback => callback(), reply);

    internal async ValueTask<bool> TryInvokeAsync<TState>(Action<TState> reply, TState state)
    {
        ArgumentNullException.ThrowIfNull(reply);
        if (!await _lane.RunAsync(static self => self.BeginReply(), this).ConfigureAwait(false))
            return false;
        try
        {
            reply(state);
        }
        finally
        {
            var completed = await _lane
                .RunAsync(static self => self.CompleteReply(), this)
                .ConfigureAwait(false);
            completed?.TrySetResult();
        }
        return true;
    }

    internal async ValueTask CloseAsync()
    {
        var wait = await _lane
            .RunAsync(() =>
            {
                _open = false;
                if (_activeReplies == 0)
                    return (Task?)null;
                _repliesDrained ??= new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                return _repliesDrained.Task;
            })
            .ConfigureAwait(false);
        if (wait is not null)
            await wait.ConfigureAwait(false);
    }

    private bool BeginReply()
    {
        if (!_open)
            return false;
        _activeReplies++;
        return true;
    }

    private TaskCompletionSource? CompleteReply()
    {
        _activeReplies--;
        if (_activeReplies == 0)
        {
            var completed = _repliesDrained;
            _repliesDrained = null;
            return completed;
        }
        return null;
    }

    internal bool TryInvoke(Action reply) => AwaitStateLane(TryInvokeAsync(reply));

    internal bool TryInvoke<TState>(Action<TState> reply, TState state) =>
        AwaitStateLane(TryInvokeAsync(reply, state));

    internal void Close() => AwaitStateLane(CloseAsync());
}
