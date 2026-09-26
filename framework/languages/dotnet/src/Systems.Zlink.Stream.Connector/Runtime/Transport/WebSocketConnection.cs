using System.Buffers;
using System.Net.WebSockets;

namespace Systems.Zlink.Stream.Connector.Runtime.Transport;

internal sealed class WebSocketConnection(ClientWebSocket webSocket, int maxReceivePayloadSize)
    : IZlinkStreamConnection
{
    private readonly long _maxReceiveFrameSize = ZlinkStreamFrameCodec.GetMaxReceiveFrameSize(
        maxReceivePayloadSize
    );

    private readonly byte[] _receiveBuffer = new byte[8192];
    private int _pendingLength;
    private byte[]? _pendingMessage;
    private int _pendingOffset;

    public bool CanWriteSegments => false;

    public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken)
    {
        while (_pendingMessage is null)
        {
            var message = ArrayPool<byte>.Shared.Rent(_receiveBuffer.Length);
            var messageLength = 0;
            WebSocketReceiveResult result;
            try
            {
                do
                {
                    result = await webSocket
                        .ReceiveAsync(_receiveBuffer, cancellationToken)
                        .ConfigureAwait(false);
                    if (result.MessageType == WebSocketMessageType.Close)
                    {
                        ArrayPool<byte>.Shared.Return(message);
                        return 0;
                    }

                    if (result.MessageType != WebSocketMessageType.Binary)
                        throw ZlinkStreamConnector.Error(
                            ZlinkStreamErrorCode.FrameDecodeFailed,
                            "WebSocket text messages are not supported."
                        );

                    var requiredCapacity = (long)messageLength + result.Count;
                    if (requiredCapacity > _maxReceiveFrameSize)
                        throw ZlinkStreamConnector.Error(
                            ZlinkStreamErrorCode.FrameTooLarge,
                            "WebSocket message exceeds MaxReceivePayloadSize."
                        );

                    EnsureCapacity(ref message, messageLength, (int)requiredCapacity);
                    _receiveBuffer.AsSpan(0, result.Count).CopyTo(message.AsSpan(messageLength));
                    messageLength += result.Count;
                } while (!result.EndOfMessage);
            }
            catch
            {
                ArrayPool<byte>.Shared.Return(message);
                throw;
            }

            if (messageLength == 0)
            {
                ArrayPool<byte>.Shared.Return(message);
                continue;
            }

            _pendingMessage = message;
            _pendingLength = messageLength;
            _pendingOffset = 0;
        }

        var remaining = _pendingLength - _pendingOffset;
        var count = Math.Min(buffer.Length, remaining);
        _pendingMessage.AsMemory(_pendingOffset, count).CopyTo(buffer);
        _pendingOffset += count;
        if (_pendingOffset == _pendingLength)
        {
            ArrayPool<byte>.Shared.Return(_pendingMessage);
            _pendingMessage = null;
            _pendingLength = 0;
            _pendingOffset = 0;
        }

        return count;
    }

    public async ValueTask WriteAsync(
        ReadOnlyMemory<byte> buffer,
        CancellationToken cancellationToken
    )
    {
        await webSocket
            .SendAsync(buffer, WebSocketMessageType.Binary, true, cancellationToken)
            .ConfigureAwait(false);
    }

    /// <summary>
    ///     Aborts the socket. A frame being written fails, and nothing waits for the peer to
    ///     read or to answer a close frame (stream-connector spec §7).
    /// </summary>
    /// <remarks>
    ///     A close handshake is not attempted: <c>CloseOutputAsync</c> queues behind a send in
    ///     progress and waits while its own close frame is written, so a peer that stops
    ///     reading would hold <c>Close</c> open. A message the receive loop still holds is
    ///     left to the collector rather than returned to the pool, because the receive loop
    ///     may be copying from it on another thread.
    /// </remarks>
    public ValueTask CloseAsync(CancellationToken cancellationToken)
    {
        webSocket.Dispose();
        return default;
    }

    private static void EnsureCapacity(ref byte[] buffer, int existingLength, int requiredCapacity)
    {
        if (buffer.Length >= requiredCapacity)
            return;

        var newLength = buffer.Length;
        while (newLength < requiredCapacity)
            newLength = checked(newLength * 2);

        var next = ArrayPool<byte>.Shared.Rent(newLength);
        buffer.AsSpan(0, existingLength).CopyTo(next);
        ArrayPool<byte>.Shared.Return(buffer);
        buffer = next;
    }
}
