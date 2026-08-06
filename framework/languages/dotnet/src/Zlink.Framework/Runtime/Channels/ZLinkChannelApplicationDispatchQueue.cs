using System.Threading.Channels;

using Zlink.Framework.Runtime.Dispatch;
namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelApplicationDispatchQueue<TWork> : IAsyncDisposable
{
    private const int Capacity = 1024;
    private static readonly TimeSpan ShutdownJoinTimeout =
        TimeSpan.FromSeconds(1);
    private readonly Channel<DispatchWork<TWork>> _queue =
        Channel.CreateBounded<DispatchWork<TWork>>(
        new BoundedChannelOptions(Capacity)
        {
            FullMode = BoundedChannelFullMode.Wait,
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
        ZLinkInboundDispatchBudget budget,
        ulong payloadBytes,
        CancellationToken cancellationToken,
        bool overageReservation = false)
    {
        ArgumentNullException.ThrowIfNull(budget);
        var work = new DispatchWork<TWork>(
            payload,
            budget,
            payloadBytes,
            overageReservation);
        if (Volatile.Read(ref _stopped) != 0)
        {
            Reject(work);
            return ValueTask.FromResult(false);
        }

        if (cancellationToken.IsCancellationRequested
            || !_queue.Writer.TryWrite(work))
        {
            // The receive loop must not wait for application dispatch space.
            // A full application queue is observable through the supplied
            // rejection path, while infrastructure/control receives continue.
            Reject(work);
            return ValueTask.FromResult(false);
        }

        return ValueTask.FromResult(true);
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _stopped, 1) != 0)
            return;

        ReplyGate.Close();
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
                var handlerStarted = false;
                try
                {
                    work.Budget.HandlerStarted(work.PayloadBytes);
                    handlerStarted = true;
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
                finally
                {
                    work.Budget.Completed(
                        work.PayloadBytes,
                        handlerStarted,
                        work.OverageReservation);
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
        finally
        {
            work.Budget.Completed(
                work.PayloadBytes,
                handlerStarted: false,
                overageReservation: work.OverageReservation);
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

    private readonly record struct DispatchWork<TPayload>(
        TPayload Payload,
        ZLinkInboundDispatchBudget Budget,
        ulong PayloadBytes,
        bool OverageReservation);
}

internal sealed class ZLinkChannelReplyGate
{
    private readonly object _gate = new();
    private bool _open = true;

    internal bool TryInvoke(Action reply)
    {
        ArgumentNullException.ThrowIfNull(reply);
        lock (_gate)
        {
            if (!_open)
                return false;
            reply();
            return true;
        }
    }

    internal void Close()
    {
        lock (_gate)
            _open = false;
    }
}
