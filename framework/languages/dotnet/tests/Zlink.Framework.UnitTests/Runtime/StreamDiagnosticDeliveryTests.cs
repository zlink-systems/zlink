using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests;

public sealed class StreamDiagnosticDeliveryTests
{
    private const string DeliveryBarrier = "stream-diagnostic-delivery-barrier";

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Stream_reader_makes_progress_while_diagnostic_provider_is_held(
        bool failCreation
    )
    {
        using var sink = new BlockingProvider(failCreation);
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Logging.AddProvider(sink);
        builder.Logging.SetMinimumLevel(LogLevel.Debug);
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options
                .AddStreamNode("diagnostic-delivery")
                .Bind("tcp://127.0.0.1:0")
                .AddSession<FailingSession>();
        });
        using var host = builder.Build();
        await host.StartAsync();
        try
        {
            var runtime = host.Services.GetRequiredService<IZLinkFrameworkRuntime>();
            var endpoint = new Uri(
                runtime.GetListenerStatus(ZLinkListenerKind.Stream, "diagnostic-delivery").Endpoint
            );
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            using var peer = new TcpClient();
            await peer.ConnectAsync(endpoint.Host, endpoint.Port, deadline.Token);
            var header = new ZlinkStreamHeader(
                failCreation ? ZlinkStreamMessageKind.Send : ZlinkStreamMessageKind.Control,
                failCreation ? ZlinkStreamCodec.Json : ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                failCreation ? "CreateSession" : ZlinkStreamControlProtocol.HeartbeatPingName,
                ZlinkStreamMetadata.Empty
            );
            var transport = peer.GetStream();
            await transport.WriteAsync(
                ZLinkStreamFrameCodec.Encode(
                    ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
                    failCreation ? "{}"u8.ToArray() : new byte[] { 1 }
                ),
                deadline.Token
            );
            await sink.Entered.Task.WaitAsync(deadline.Token);

            using var progressDeadline = new CancellationTokenSource(TimeSpan.FromSeconds(2));
            var prefix = new byte[6];
            var progress = await Record.ExceptionAsync(async () =>
                await transport.ReadExactlyAsync(prefix, progressDeadline.Token)
            );
            Assert.True(
                progress is null,
                $"STREAM {(failCreation ? "session rejection" : "heartbeat pong")} did not progress while the diagnostic provider was held: {progress}"
            );
            var headerLength = BinaryPrimitives.ReadUInt16BigEndian(prefix);
            var payloadLength = BinaryPrimitives.ReadUInt32BigEndian(prefix.AsSpan(2));
            var body = new byte[checked(headerLength + (int)payloadLength)];
            await transport.ReadExactlyAsync(body, deadline.Token);
            var response = ZLinkStreamProtocolDefaults.DecodeHeader(body.AsMemory(0, headerLength));
            var expected = failCreation
                ? ZlinkStreamSessionClosingCodec.CreateHeader().Name
                : ZlinkStreamControlProtocol.HeartbeatPongName;
            Assert.Equal(expected, response.Name);

            host.Services.GetRequiredService<ZLinkFrameworkRuntime>()
                .Flow.Trace(
                    new ZLinkMessageFlowEvent(
                        ZLinkMessageFlowOutcome.Received,
                        ZLinkDispatchErrorSurface.StreamSession,
                        ZLinkDispatchMessageKind.Control,
                        PacketName: DeliveryBarrier
                    )
                );
            sink.Release.Set();
            var telemetryThread = await sink.BarrierThread.Task.WaitAsync(deadline.Token);
            var entry = Assert.Single(sink.Records);
            Assert.Equal(failCreation ? LogLevel.Error : LogLevel.Debug, entry.Level);
            Assert.Equal(failCreation, entry.Failure is not null);
            Assert.Equal(telemetryThread, entry.Thread);
        }
        finally
        {
            sink.Release.Set();
            await host.StopAsync();
        }
    }

    private sealed class FailingSession : IZLinkSession
    {
        public FailingSession(IZLinkSessionContext context)
        {
            Context = context;
            throw new InvalidOperationException(
                "Diagnostic delivery test session creation failed."
            );
        }

        public IZLinkSessionContext Context { get; }

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    private sealed class BlockingProvider(bool failCreation) : ILoggerProvider, ILogger
    {
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource<int> BarrierThread { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public ManualResetEventSlim Release { get; } = new();
        public ConcurrentQueue<(LogLevel Level, Exception? Failure, int Thread)> Records { get; } =
            new();

        public ILogger CreateLogger(string categoryName) => this;

        public bool IsEnabled(LogLevel level) => true;

        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public void Log<TState>(
            LogLevel level,
            EventId eventId,
            TState state,
            Exception? failure,
            Func<TState, Exception?, string> formatter
        )
        {
            if (
                state is IReadOnlyList<KeyValuePair<string, object?>> fields
                && fields.Any(field =>
                    field.Key == "packet" && Equals(field.Value, DeliveryBarrier)
                )
            )
            {
                BarrierThread.TrySetResult(Environment.CurrentManagedThreadId);
                return;
            }

            var prefix = failCreation
                ? "STREAM session creation failed for node"
                : "Ignored STREAM control name=$zlink.heartbeat.ping";
            if (!formatter(state, failure).StartsWith(prefix, StringComparison.Ordinal))
                return;
            Records.Enqueue((level, failure, Environment.CurrentManagedThreadId));
            Entered.TrySetResult();
            Release.Wait();
        }

        public void Dispose() => Release.Dispose();
    }
}
