using System.Threading.Channels;

namespace Systems.Zlink.Stream.Connector.Runtime;

/// <summary>
///     Frame write queue shared by <c>Send</c> and <c>Request</c>. It writes frames one at a time and completes each
///     operation once its frame is written to the transport (stream-connector spec §5.2).
///     An operation that waits follows its own timeout or cancellation.
/// </summary>
internal sealed class ZlinkStreamOneWaySubmitQueue
{
    private const int Capacity = 4096;
    private readonly Task _completion;
    private readonly Channel<SubmitItem> _queue;
    private readonly Func<IZlinkStreamConnection?> _connectionProvider;
    private readonly Func<
        IZlinkStreamConnection,
        ZlinkStreamOutboundFrame,
        CancellationToken,
        ValueTask
    > _sendAsync;
    private readonly object _gate = new();
    private bool _accepting = true;

    public ZlinkStreamOneWaySubmitQueue(
        ZlinkStreamTaskRunner taskRunner,
        Func<IZlinkStreamConnection?> connectionProvider,
        Func<
            IZlinkStreamConnection,
            ZlinkStreamOutboundFrame,
            CancellationToken,
            ValueTask
        > sendAsync
    )
    {
        _connectionProvider = connectionProvider;
        _sendAsync = sendAsync;
        _queue = Channel.CreateBounded<SubmitItem>(
            new BoundedChannelOptions(Capacity)
            {
                FullMode = BoundedChannelFullMode.Wait,
                SingleReader = true,
                SingleWriter = false,
                AllowSynchronousContinuations = false,
            }
        );
        _completion = taskRunner.Run(DrainAsync);
    }

    /// <summary>
    ///     Accepts <paramref name="frame" /> and completes once it is written to the
    ///     transport. Acceptance failures throw synchronously; waiting for the write is
    ///     asynchronous.
    /// </summary>
    public ValueTask SendAsync(ZlinkStreamOutboundFrame frame, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var connection = GetConnection();
        return SendAcceptedAsync(connection, frame, cancellationToken);
    }

    private async ValueTask SendAcceptedAsync(
        IZlinkStreamConnection connection,
        ZlinkStreamOutboundFrame frame,
        CancellationToken cancellationToken
    )
    {
        var written = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        try
        {
            await _queue
                .Writer.WriteAsync(
                    new SubmitItem(connection, frame, written, cancellationToken),
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch (ChannelClosedException)
        {
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.Disconnected,
                "Connector is closed."
            );
        }
        await written.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    public ValueTask SubmitRequestAsync(
        ZlinkStreamOutboundFrame frame,
        CancellationToken cancellationToken,
        Action onAccepted
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var written = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        lock (_gate)
        {
            var connection = GetConnection();
            if (
                !_queue.Writer.TryWrite(
                    new SubmitItem(connection, frame, written, cancellationToken)
                )
            )
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.SendFailed,
                    "Connector outbound frame queue is full."
                );
            onAccepted();
        }
        return new ValueTask(written.Task.WaitAsync(cancellationToken));
    }

    /// <summary>
    ///     Stops accepting. Operations accepted before this call still reach a terminal: a
    ///     frame whose connection has ended fails without being written.
    /// </summary>
    public void Complete()
    {
        lock (_gate)
        {
            _accepting = false;
            _queue.Writer.TryComplete();
        }
    }

    public ValueTask WaitForCompletionAsync() => new(_completion);

    private async ValueTask DrainAsync(CancellationToken cancellationToken)
    {
        await foreach (
            var item in _queue.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false)
        )
        {
            try
            {
                if (item.CancellationToken.IsCancellationRequested)
                {
                    item.Written.TrySetCanceled(item.CancellationToken);
                    continue;
                }

                // A frame is written only to the connection that accepted it. Once that
                // connection has ended (a transport loss or Close) the frame is not written
                // and its operation fails (stream-connector spec §7).
                if (!ReferenceEquals(item.Connection, _connectionProvider()))
                {
                    item.Written.TrySetException(
                        ZlinkStreamConnector.Error(
                            ZlinkStreamErrorCode.Disconnected,
                            "The accepted connection ended before this frame was written."
                        )
                    );
                    continue;
                }

                // Once a frame starts writing, connector lifetime owns the write.
                // Caller cancellation must not interrupt a partially written frame.
                await _sendAsync(item.Connection, item.Frame, cancellationToken)
                    .ConfigureAwait(false);
                item.Written.TrySetResult(true);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                item.Written.TrySetCanceled(cancellationToken);
                return;
            }
            catch (ZlinkStreamException exception)
            {
                // The connector's send path classifies the failure and updates lifecycle
                // state before returning the exception here.
                item.Written.TrySetException(exception);
            }
            catch (Exception exception)
            {
                item.Written.TrySetException(
                    ZlinkStreamConnector.Error(
                        ZlinkStreamErrorCode.SendFailed,
                        "Stream frame write failed.",
                        exception
                    )
                );
            }
        }
    }

    private IZlinkStreamConnection GetConnection()
    {
        return (_accepting ? _connectionProvider() : null)
            ?? throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.Disconnected,
                "Connector is not connected."
            );
    }

    private sealed record SubmitItem(
        IZlinkStreamConnection Connection,
        ZlinkStreamOutboundFrame Frame,
        TaskCompletionSource<bool> Written,
        CancellationToken CancellationToken
    );
}
