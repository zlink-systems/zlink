using System.Threading.Channels;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamOneWaySubmitQueue
{
    private const int Capacity = 4096;
    private readonly ZlinkStreamConnectorCallbacks _callbacks;
    private readonly Task _completion;
    private readonly Channel<SubmitItem> _queue;
    private readonly Func<ZlinkStreamOutboundFrame, CancellationToken, ValueTask> _sendAsync;
    private int _accepting = 1;

    public ZlinkStreamOneWaySubmitQueue(
        ZlinkStreamTaskRunner taskRunner,
        ZlinkStreamConnectorCallbacks callbacks,
        Func<ZlinkStreamOutboundFrame, CancellationToken, ValueTask> sendAsync)
    {
        _callbacks = callbacks;
        _sendAsync = sendAsync;
        _queue = Channel.CreateBounded<SubmitItem>(new BoundedChannelOptions(Capacity)
        {
            FullMode = BoundedChannelFullMode.Wait,
            SingleReader = true,
            SingleWriter = false,
            AllowSynchronousContinuations = false
        });
        _completion = taskRunner.Run(DrainAsync);
    }

    public async ValueTask SubmitAsync(
        ZlinkStreamOutboundFrame frame,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (Volatile.Read(ref _accepting) == 0)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.SendFailed,
                "Connector is not accepting one-way sends.");
        try
        {
            await _queue.Writer.WriteAsync(
                    new SubmitItem(frame, null, CancellationToken.None),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ChannelClosedException)
        {
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.SendFailed,
                "Connector is not accepting one-way sends.");
        }
    }

    public async ValueTask SendAsync(
        ZlinkStreamOutboundFrame frame,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (Volatile.Read(ref _accepting) == 0)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.SendFailed,
                "Connector is not accepting outbound frames.");

        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_queue.Writer.TryWrite(new SubmitItem(frame, completion, cancellationToken)))
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.SendFailed,
                "Connector outbound frame queue is full.");

        await completion.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    public void Complete()
    {
        if (Interlocked.Exchange(ref _accepting, 0) == 0) return;
        _queue.Writer.TryComplete();
    }

    public ValueTask WaitForCompletionAsync() => new(_completion);

    private async ValueTask DrainAsync(CancellationToken cancellationToken)
    {
        await foreach (var item in _queue.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
        {
            if (item.CancellationToken.IsCancellationRequested)
            {
                item.Completion?.TrySetCanceled(item.CancellationToken);
                continue;
            }

            try
            {
                // Once a frame starts writing, connector lifetime owns the write.
                // Caller cancellation must not interrupt a partially written frame.
                await _sendAsync(item.Frame, cancellationToken).ConfigureAwait(false);
                item.Completion?.TrySetResult();
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                item.Completion?.TrySetCanceled(cancellationToken);
                return;
            }
            catch (ZlinkStreamException exception)
            {
                // SendFrameAsync publishes the transport failure and updates
                // lifecycle state before returning the exception here.
                item.Completion?.TrySetException(exception);
            }
            catch (Exception exception)
            {
                item.Completion?.TrySetException(exception);
                await _callbacks.PublishErrorAsync(
                        new ZlinkStreamError(
                            ZlinkStreamErrorCode.SendFailed,
                            "Accepted one-way stream send failed.",
                            exception),
                        CancellationToken.None)
                    .ConfigureAwait(false);
            }
        }
    }

    private sealed record SubmitItem(
        ZlinkStreamOutboundFrame Frame,
        TaskCompletionSource? Completion,
        CancellationToken CancellationToken);
}
