using System.Buffers.Binary;
using System.Threading.Channels;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Framing;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit;

namespace Zlink.Framework.UnitTests;

public sealed class StreamFrameWriteTests
{
    [Fact]
    public async Task Each_Frame_Is_One_Complete_Transport_Write_And_Awaits_Completion()
    {
        var connection = new HeldWriteConnection();
        using var sendGate = new SemaphoreSlim(1, 1);
        var sender = new ZlinkStreamFrameSender(
            new ZlinkStreamConnectorOptions { Endpoint = new Uri("tcp://127.0.0.1:1") },
            new ZlinkStreamHeaderCodec(),
            null,
            sendGate,
            () => connection
        );
        var frames = new (byte[] Header, byte[] Payload)[]
        {
            (new byte[] { 1, 2, 3, 4 }, Enumerable.Repeat((byte)'x', 1024).ToArray()),
            (new byte[] { 5, 6, 7 }, new byte[] { 8, 9 }),
        };

        for (var index = 0; index < frames.Length; index++)
        {
            connection.HoldWrites();
            var (header, payload) = frames[index];
            var sending = sender
                .SendPacketAsync(connection, header, payload, CancellationToken.None)
                .AsTask();
            try
            {
                var written = await connection
                    .Writes.Reader.ReadAsync()
                    .AsTask()
                    .WaitAsync(TimeSpan.FromSeconds(5));
                // #1382: splitting prefix, header and payload into separate writes let
                // Nagle hold the frame tail until the peer's TCP delayed ACK.
                var prefixSize = ZlinkStreamFrameCodec.PrefixSize;
                Assert.Equal(prefixSize + header.Length + payload.Length, written.Length);
                Assert.Equal(header.Length, BinaryPrimitives.ReadUInt16BigEndian(written));
                Assert.Equal(
                    (uint)payload.Length,
                    BinaryPrimitives.ReadUInt32BigEndian(written.AsSpan(2, 4))
                );
                Assert.Equal(header, written.AsSpan(prefixSize, header.Length).ToArray());
                Assert.Equal(payload, written.AsSpan(prefixSize + header.Length).ToArray());
                Assert.Equal(index + 1, connection.WriteCount);
                Assert.False(sending.IsCompleted);
            }
            finally
            {
                connection.ReleaseWrites();
                await sending.WaitAsync(TimeSpan.FromSeconds(5));
            }

            Assert.Equal(index + 1, connection.WriteCount);
        }
    }

    private sealed class HeldWriteConnection : IZlinkStreamConnection
    {
        private TaskCompletionSource _writeCompletion = NewCompletion();
        private int _writeCount;

        public Channel<byte[]> Writes { get; } = Channel.CreateUnbounded<byte[]>();

        public int WriteCount => Volatile.Read(ref _writeCount);

        // The pre-698d8cdf0c transport interface used this property to select the
        // segmented TCP write path. Keep the same test usable with that source.
        public bool CanWriteSegments => true;

        public void HoldWrites() => _writeCompletion = NewCompletion();

        public void ReleaseWrites() => _writeCompletion.TrySetResult();

        public ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken
        )
        {
            var completion = _writeCompletion.Task;
            Interlocked.Increment(ref _writeCount);
            Writes.Writer.TryWrite(buffer.ToArray());
            return new ValueTask(completion.WaitAsync(cancellationToken));
        }

        public ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken) =>
            throw new NotSupportedException();

        public ValueTask CloseAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

        private static TaskCompletionSource NewCompletion() =>
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }
}
