using System.Collections.Concurrent;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Framing;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task UnmatchedResponseDoesNotDecompressItsPayload()
    {
        var connection = new ScriptedConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Immediate);
        var errors = new ConcurrentQueue<ZlinkStreamErrorCode>();
        connector.OnErrorReceived(
            (error, _) =>
            {
                errors.Enqueue(error.Code);
                return ValueTask.CompletedTask;
            }
        );
        var nextPacket = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.On(
            "after.unmatched",
            (_, _) =>
            {
                nextPacket.TrySetResult();
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            var codec = new ZlinkStreamHeaderCodec();
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq | ZlinkStreamHeaderFlags.PayloadCompressed,
                new ZlinkStreamRequestSeq(918273),
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            connection.Deliver(
                ZlinkStreamFrameCodec.Encode(codec.Encode(header).Span, CorruptCompressedPayload())
            );
            connection.Deliver(PushFrame("after.unmatched", [1], compressed: false));

            await nextPacket.Task.WaitAsync(EndingWait);
            Assert.Empty(errors);
            Assert.Equal(ZlinkStreamConnectionState.Connected, connector.State);
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    [Theory]
    [InlineData("{\"code\":\"x\"}")]
    [InlineData("{\"code\":\"x\",\"message\":42}")]
    public async Task ErrorWithoutStringMessageIsFrameDecodeFailed(string payload)
    {
        var connection = new ScriptedConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Immediate);
        var received = new TaskCompletionSource<ZlinkStreamErrorCode>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.OnErrorReceived(
            (error, _) =>
            {
                received.TrySetResult(error.Code);
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            var codec = new ZlinkStreamHeaderCodec();
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Error,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.None,
                null,
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            connection.Deliver(
                ZlinkStreamFrameCodec.Encode(
                    codec.Encode(header).Span,
                    System.Text.Encoding.UTF8.GetBytes(payload)
                )
            );

            Assert.Equal(
                ZlinkStreamErrorCode.FrameDecodeFailed,
                await received.Task.WaitAsync(EndingWait)
            );
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    [Theory]
    [InlineData("heartbeat-interval")]
    [InlineData("heartbeat-timeout")]
    [InlineData("reconnect-initial")]
    [InlineData("reconnect-max")]
    [InlineData("reconnect-attempts")]
    [InlineData("reconnect-nan")]
    [InlineData("reconnect-infinity")]
    public void InvalidInactiveOptionsAreRejectedBeforeConnection(string invalid)
    {
        var heartbeat = invalid switch
        {
            "heartbeat-interval" => new ZlinkStreamHeartbeatOptions
            {
                Enabled = false,
                Interval = TimeSpan.Zero,
            },
            "heartbeat-timeout" => new ZlinkStreamHeartbeatOptions
            {
                Enabled = false,
                Timeout = TimeSpan.Zero,
            },
            _ => new ZlinkStreamHeartbeatOptions { Enabled = false },
        };
        var reconnect = invalid switch
        {
            "reconnect-initial" => new ZlinkStreamReconnectOptions
            {
                Enabled = false,
                InitialDelay = TimeSpan.Zero,
            },
            "reconnect-max" => new ZlinkStreamReconnectOptions
            {
                Enabled = false,
                MaxDelay = TimeSpan.Zero,
            },
            "reconnect-attempts" => new ZlinkStreamReconnectOptions
            {
                Enabled = false,
                MaxAttempts = 0,
            },
            "reconnect-nan" => new ZlinkStreamReconnectOptions
            {
                Enabled = false,
                BackoffFactor = double.NaN,
            },
            "reconnect-infinity" => new ZlinkStreamReconnectOptions
            {
                Enabled = false,
                BackoffFactor = double.PositiveInfinity,
            },
            _ => new ZlinkStreamReconnectOptions { Enabled = false },
        };
        var error = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri("tcp://127.0.0.1:1"),
                    Heartbeat = heartbeat,
                    Reconnect = reconnect,
                }
            )
        );
        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, error.Error.Code);
    }

    [Fact]
    public void PositiveOptionsHaveNoOrderingConstraint()
    {
        var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions
                {
                    Interval = TimeSpan.FromSeconds(5),
                    Timeout = TimeSpan.FromSeconds(1),
                },
                Reconnect = new ZlinkStreamReconnectOptions
                {
                    InitialDelay = TimeSpan.FromMilliseconds(500),
                    MaxDelay = TimeSpan.FromMilliseconds(100),
                    BackoffFactor = 0.5,
                },
            }
        );
        Assert.NotNull(connector);
    }
}
