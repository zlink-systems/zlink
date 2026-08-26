using System.Threading.Channels;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelApplicationDispatchQueue<TWork> : IAsyncDisposable
{
    private static readonly TimeSpan ShutdownJoinTimeout =
        TimeSpan.FromSeconds(1);
    private readonly Channel<DispatchWork<TWork>> _queue =
        Channel.CreateUnbounded<DispatchWork<TWork>>(
            new UnboundedChannelOptions
            {
                SingleReader = true,
                SingleWriter = true,
                AllowSynchronousContinuations = false
            });
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly CancellationTokenSource _stop;
    private readonly string _name;
    private readonly Task _worker;
    private readonly Func<TWork, CancellationToken, ValueTask> _dispatch;
    private readonly Action<TWork> _reject;
    private int _stopped;

    internal ZLinkChannelReplyGate ReplyGate { get; } = new();

    internal ZLinkChannelApplicationDispatchQueue(
        string name,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken laneCancellationToken,
        Func<TWork, CancellationToken, ValueTask> dispatch,
        Action<TWork> reject)
    {
        _name = name;
        _errorSink = errorSink;
        _dispatch = dispatch ?? throw new ArgumentNullException(nameof(dispatch));
        _reject = reject ?? throw new ArgumentNullException(nameof(reject));
        _stop = CancellationTokenSource.CreateLinkedTokenSource(
            laneCancellationToken);
        _worker = Task.Run(
            () => RunAsync(_stop.Token).AsTask(),
            CancellationToken.None);
    }

    internal ValueTask<bool> PostAsync(
        TWork payload,
        CancellationToken cancellationToken)
    {
        var work = new DispatchWork<TWork>(payload);
        if (Volatile.Read(ref _stopped) != 0)
        {
            Reject(work);
            return ValueTask.FromResult(false);
        }

        if (cancellationToken.IsCancellationRequested
            || !_queue.Writer.TryWrite(work))
        {
            Reject(work);
            return ValueTask.FromResult(false);
        }

        return ValueTask.FromResult(true);
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _stopped, 1) != 0)
            return;

        await ReplyGate.CloseAsync().ConfigureAwait(false);
        _queue.Writer.TryComplete();
        await _stop.CancelAsync().ConfigureAwait(false);

        var completed = await Task.WhenAny(
                _worker,
                Task.Delay(ShutdownJoinTimeout))
            .ConfigureAwait(false);
        if (ReferenceEquals(completed, _worker))
        {
            await _worker.ConfigureAwait(false);
            _stop.Dispose();
            return;
        }

        _ = _worker.ContinueWith(
            static (_, state) =>
                ((CancellationTokenSource)state!).Dispose(),
            _stop,
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    private async ValueTask RunAsync(CancellationToken cancellationToken)
    {
        try
        {
            await foreach (var work in _queue.Reader.ReadAllAsync(cancellationToken)
                               .ConfigureAwait(false))
            {
                if (cancellationToken.IsCancellationRequested)
                {
                    Reject(work);
                    while (_queue.Reader.TryRead(out var pending))
                        Reject(pending);
                    break;
                }
                try
                {
                    await _dispatch(work.Payload, cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                    when (cancellationToken.IsCancellationRequested)
                {
                }
                catch (Exception exception)
                {
                    Report(exception);
                }
            }
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
        }
        finally
        {
            Interlocked.Exchange(ref _stopped, 1);
            _queue.Writer.TryComplete();
            while (_queue.Reader.TryRead(out var pending)) Reject(pending);
        }
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
        catch
        {
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

    internal async ValueTask<bool> TryInvokeAsync(Action reply)
    {
        ArgumentNullException.ThrowIfNull(reply);
        if (!await _lane.RunAsync(BeginReply).ConfigureAwait(false))
            return false;
        try
        {
            reply();
        }
        finally
        {
            var completed = await _lane.RunAsync(CompleteReply)
                .ConfigureAwait(false);
            completed?.TrySetResult();
        }
        return true;
    }

    internal async ValueTask CloseAsync()
    {
        var wait = await _lane.RunAsync(() =>
        {
            _open = false;
            if (_activeReplies == 0)
                return (Task?)null;
            _repliesDrained ??= new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            return _repliesDrained.Task;
        }).ConfigureAwait(false);
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

    internal bool TryInvoke(Action reply) =>
        AwaitStateLane(TryInvokeAsync(reply));

    internal void Close() =>
        AwaitStateLane(CloseAsync());

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();
}
