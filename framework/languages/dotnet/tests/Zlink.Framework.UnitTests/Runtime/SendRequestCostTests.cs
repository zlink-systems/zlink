using System.Diagnostics;
using System.Runtime.CompilerServices;
using Microsoft.Extensions.DependencyInjection;
using Xunit.Abstractions;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Codecs;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SendRequestCostTests(ITestOutputHelper output)
{
    private const int Iterations = 1000;
    private static readonly TimeSpan Budget = TimeSpan.FromSeconds(1);
    private static readonly Probe Payload = new();
    private static readonly byte[] Body = new byte[1024];
    private static readonly IReadOnlyDictionary<string, string> Metadata = new Dictionary<
        string,
        string
    >
    {
        ["key"] = "value",
    };
    private static readonly IReadOnlyList<Message> ReplyParts = [];
    private static readonly Task<IReadOnlyList<Message>> Reply = Task.FromResult(ReplyParts);

    [Fact]
    public async Task Channel_send_constructs_one_header_before_serialization()
    {
        var serializer = new PreambleSerializer();
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
            options.AddClientServerChannel("work").Client().Connect("tcp://127.0.0.1:1")
        );
        await using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();
        registration.Codecs.AddSerializer(
            "application/x-cost",
            serializer,
            static type => type == typeof(Probe)
        );
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var activation = new Activation(runtime.Flow, registration.Codecs);
        activation.OutboundEndpoint = new ZLinkSpotOutboundEndpoint(activation, null!, runtime);

        // Stop in the serializer so the measured interval contains only the
        // call's header preparation, without a transport or exception allocation.
        for (var index = 0; index < 128; index++)
            await ObservePreamble();
        long actual = 0;
        long baseline = 0;
        var accepted = 0;
        for (var index = 0; index < Iterations; index++)
        {
            var collection = Collections();
            var thread = Environment.CurrentManagedThreadId;
            var measured = await ObservePreamble();
            var started = GC.GetAllocatedBytesForCurrentThread();
            GC.KeepAlive(
                new ZLinkEnvelopeHeader(
                    ZLinkMessageKind.Command,
                    "work",
                    nameof(Probe),
                    ZLinkEnvelopeCodec.DefaultContentType,
                    null,
                    null,
                    null,
                    null,
                    null
                )
                {
                    Metadata = ZLinkMessageMetadata.Empty.Values,
                }
            );
            var control = GC.GetAllocatedBytesForCurrentThread() - started;
            if (collection != Collections() || thread != Environment.CurrentManagedThreadId)
                continue;
            actual += measured;
            baseline += control;
            accepted++;
        }
        Assert.True(accepted > 0);
        output.WriteLine(
            $"channel send preamble {actual / accepted} B/op; single header {baseline / accepted} B/op; accepted {accepted}"
        );
        Assert.Equal(baseline, actual);

        async ValueTask<long> ObservePreamble()
        {
            var call = new ZLinkCurrentSpotSendCall<Probe>(activation, "work", Payload);
            serializer.Started = GC.GetAllocatedBytesForCurrentThread();
            var pending = call.Async();
            var allocated = serializer.Allocated;
            await Assert.ThrowsAsync<ProbeEnd>(() => pending.AsTask());
            return allocated;
        }
    }

    [Theory]
    [InlineData("packet{literal}", "Node request failed for 'packet{literal}'.")]
    [InlineData(null, "literal {unformatted} error")]
    public void Reply_error_formats_only_an_explicit_argument(string? packet, string expected)
    {
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Error,
                "work",
                nameof(Probe),
                ZLinkEnvelopeCodec.DefaultContentType,
                "request",
                null,
                null,
                "protocol_error",
                null
            )
        );
        var error = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkEnvelopeReplyDecoder.Decode<Probe>(
                [encoded],
                "empty",
                packet is null ? expected : "Node request failed for '{0}'.",
                null,
                errorMessageArgument: packet
            )
        );
        Assert.Equal(expected, error.Message);
    }

    [Fact]
    public void Header_initialization_keeps_metadata_and_the_wire_payload()
    {
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Command,
            "work",
            nameof(Probe),
            metadata: Metadata
        );
        Assert.Same(Metadata, header.Metadata);
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header);
        var decoded = ZLinkEnvelopeCodec.DecodeHeader([encoded]);
        Assert.Equal("value", decoded.Metadata!["key"]);
    }

    [Fact]
    public async Task Successful_raw_submission_does_not_format_a_failure_or_allocate_a_callback()
    {
        for (var index = 0; index < 128; index++)
            await OptimizedRaw(42);
        var accepted = 0;
        for (var sample = 0; sample < 12; sample++)
        {
            var collection = Collections();
            var thread = Environment.CurrentManagedThreadId;
            var before = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < Iterations; index++)
                Assert.Same(ReplyParts, await OptimizedRaw(42));
            var actual = GC.GetAllocatedBytesForCurrentThread() - before;
            before = GC.GetAllocatedBytesForCurrentThread();
            for (var index = 0; index < Iterations; index++)
                Assert.Same(ReplyParts, await ValueTask.FromResult(ReplyParts));
            var runtime = GC.GetAllocatedBytesForCurrentThread() - before;
            if (collection != Collections() || thread != Environment.CurrentManagedThreadId)
                continue;
            Assert.Equal(runtime, actual);
            accepted++;
        }
        Assert.True(accepted > 0);
    }

    [Theory]
    [InlineData(null, "Node request failed for 'Probe'.")]
    [InlineData("remote failure", "remote failure")]
    public void Lazy_reply_failure_keeps_remote_and_fallback_messages(
        string? remoteMessage,
        string expected
    )
    {
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Error,
                "work",
                nameof(Probe),
                ZLinkEnvelopeCodec.DefaultContentType,
                "request",
                null,
                null,
                "protocol_error",
                remoteMessage
            )
        );
        var error = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkEnvelopeReplyDecoder.Decode<Probe>(
                [encoded],
                "empty",
                "Node request failed for '{0}'.",
                null,
                errorMessageArgument: nameof(Probe)
            )
        );
        Assert.Equal(expected, error.Message);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Stateful_raw_submission_preserves_submit_and_completion_failures(
        bool completion
    )
    {
        var failure = new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkRawRequestSubmitter.SubmitAsync(
                (Completion: completion, Failure: failure),
                Array.Empty<Message>(),
                static (state, _, _, _) =>
                    state.Completion
                        ? Task.FromException<IReadOnlyList<Message>>(state.Failure)
                        : throw state.Failure,
                Budget,
                static (_, result) => $"ClientServer request failed for 'work': {result}.",
                CancellationToken.None
            )
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.Contains("ClientServer request failed for 'work': NotConnected.", error.Message);
        Assert.Same(failure, error.InnerException);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task Readiness_wait_observes_caller_and_shutdown_cancellation(
        bool shutdown,
        bool preCancelled
    )
    {
        using var caller = new CancellationTokenSource();
        using var stop = new CancellationTokenSource();
        var errors = new ZLinkRuntimeErrorSink();
        await using var client = new ZLinkClientServerClientRuntime(
            "work",
            null!,
            null!,
            null!,
            stop.Token,
            errors,
            new ZLinkRuntimeTaskRunner(errors, stop.Token)
        );
        var cancel = shutdown ? stop : caller;
        if (preCancelled)
            cancel.Cancel();
        var pending = client.RequestAsync([], Budget, caller.Token).AsTask();
        if (!preCancelled)
        {
            Assert.False(pending.IsCompleted);
            cancel.Cancel();
        }
        if (shutdown)
        {
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => pending);
            Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, failure.Kind);
        }
        else
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
        var send = client.SendAsync([], caller.Token);
        Assert.Equal(ZLinkOneWaySubmitStatus.Shutdown, (await send).Status);
    }

    [Fact]
    public void Alternating_send_request_microbench()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer(
            "application/x-cost",
            new PartSerializer(),
            static type => type == typeof(Probe)
        );
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            "work",
            nameof(Probe),
            Budget
        );
        Action binding = () =>
        {
            using var body = Message.From(Body);
        };
        Action encode = () =>
            ZLinkMessageParts.DisposeAll(
                ZLinkClientCallCodec.EncodeEnvelopeParts(header, Payload, codecs)
            );
        Action retainedBody = () =>
        {
            using var body = ZLinkEnvelopeCodec.EncodeBody(
                Payload,
                typeof(Probe),
                codecs,
                out var contentType
            );
            var parts = ZLinkMessageParts.Create(
                ZLinkEnvelopeCodec.EncodeHeader(header, contentType),
                body.Copy()
            );
            ZLinkMessageParts.DisposeAll(parts);
        };
        Compare("channel request encode", retainedBody, encode, binding);
        Compare(
            "metadata header",
            () =>
                GC.KeepAlive(
                    ZLinkClientCallCodec.CreateEnvelope(
                        ZLinkMessageKind.Command,
                        "work",
                        nameof(Probe)
                    ) with
                    {
                        Metadata = Metadata,
                    }
                ),
            () =>
                GC.KeepAlive(
                    ZLinkClientCallCodec.CreateEnvelope(
                        ZLinkMessageKind.Command,
                        "work",
                        nameof(Probe),
                        metadata: Metadata
                    )
                )
        );
        var response = ZLinkClientCallCodec.EncodeEnvelopeParts(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Response,
                "work",
                nameof(Probe),
                "application/x-cost",
                "response",
                null,
                null,
                null,
                null
            ),
            Payload,
            codecs
        );
        try
        {
            var packet = nameof(Probe);
            Compare(
                "successful reply fallback",
                () =>
                    GC.KeepAlive(
                        ZLinkEnvelopeReplyDecoder.Decode<Probe>(
                            response,
                            "empty",
                            $"Node request failed for '{packet}'.",
                            codecs,
                            false
                        )
                    ),
                () =>
                    GC.KeepAlive(
                        ZLinkEnvelopeReplyDecoder.Decode<Probe>(
                            response,
                            "empty",
                            "Node request failed for '{0}'.",
                            codecs,
                            false,
                            errorMessageArgument: packet
                        )
                    )
            );
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(response);
        }
        Compare(
            "ready wait callback scope",
            () => BaselineWaitScope(true).GetAwaiter().GetResult(),
            () => ScopedWaitScope(true).GetAwaiter().GetResult()
        );
        using var selection = new SelectionOwner();
        Compare("selector lane callback", () => selection.Bound(), () => selection.State());
        Compare(
            "raw submission",
            () => BaselineRaw(42).GetAwaiter().GetResult(),
            () => OptimizedRaw(42).GetAwaiter().GetResult()
        );
    }

    [Fact]
    public async Task Component_client_lookup_uses_the_lane_without_callback_storage()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(_ => { });
        await using var provider = services.BuildServiceProvider();
        var host = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var state = await host.EnsureStartedStateAsync(CancellationToken.None);
        var errors = new ZLinkRuntimeErrorSink();
        await using var client = new ZLinkClientServerClientRuntime(
            "cost",
            null!,
            null!,
            null!,
            CancellationToken.None,
            errors,
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None)
        );
        var manager = new ZLinkChannelRuntimeManager(
            null!,
            new ZLinkFrameworkRegistration(),
            null!,
            null!
        );
        await state.RunStateAsync(() => state.ClientServerClientRuntimes.Add("cost", client));
        try
        {
            Action actual = () => GC.KeepAlive(manager.GetClientServerClientRuntime(state, "cost"));
            Action control = () =>
                GC.KeepAlive(
                    state
                        .RunStateAsync(static () => (ZLinkClientServerClientRuntime)null!)
                        .GetAwaiter()
                        .GetResult()
                );
            for (var index = 0; index < 128; index++)
            {
                actual();
                control();
            }
            var accepted = 0;
            for (var sample = 0; sample < 12; sample++)
            {
                var collection = Collections();
                var before = GC.GetAllocatedBytesForCurrentThread();
                for (var index = 0; index < Iterations; index++)
                    control();
                var baseline = GC.GetAllocatedBytesForCurrentThread() - before;
                before = GC.GetAllocatedBytesForCurrentThread();
                for (var index = 0; index < Iterations; index++)
                    actual();
                var measured = GC.GetAllocatedBytesForCurrentThread() - before;
                if (collection != Collections())
                    continue;
                output.WriteLine(
                    $"component lookup {measured / Iterations} B/op; owner lane control {baseline / Iterations} B/op"
                );
                Assert.Equal(baseline, measured);
                accepted++;
            }
            Assert.True(accepted > 0);
            var missing = Assert.Throws<ZLinkConfigurationException>(() =>
                manager.GetClientServerClientRuntime(state, "missing")
            );
            Assert.Equal(
                "ClientServer client channel 'missing' is not initialized.",
                missing.Message
            );
            Compare(
                "component client lookup",
                () => GC.KeepAlive(BaselineLookup(state, "cost")),
                actual
            );
        }
        finally
        {
            await state.RunStateAsync(() => state.ClientServerClientRuntimes.Remove("cost"));
            await host.StopAsync(CancellationToken.None);
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static ZLinkClientServerClientRuntime BaselineLookup(
        ZLinkFrameworkComponentState state,
        string channelName
    ) =>
        state
            .RunStateAsync(() =>
            {
                if (state.ClientServerClientRuntimes.TryGetValue(channelName, out var runtime))
                    return runtime;
                throw new ZLinkConfigurationException(
                    $"ClientServer client channel '{channelName}' is not initialized."
                );
            })
            .GetAwaiter()
            .GetResult();

    [Fact]
    public async Task Alternating_ready_path_cancellation_microbench()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
            options
                .AddClientServerChannel("cost")
                .Server()
                .Listen(0)
                .SetBindHost("127.0.0.1")
                .AddSendHandler<ProbeHandler, Probe>()
        );
        await using var server = services.BuildServiceProvider();
        server
            .GetRequiredService<ZLinkFrameworkRegistration>()
            .Channels["cost"]
            .Server!.SocketConfig.MaxMessageSize = 512;
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        await serverRuntime.StartAsync(CancellationToken.None);
        var state = await serverRuntime.EnsureStartedStateAsync(CancellationToken.None);
        var endpoint = (
            await state
                .ClientServerServerBundles[ZLinkChannelName.FromBoundary("cost", "channel")]
                .ClientServerServer!.ReadAsync()
        ).AdvertisedEndpoint;
        var clientServices = new ServiceCollection();
        clientServices.AddZLinkFramework(options =>
            options.AddClientServerChannel("cost").Client().Connect(endpoint)
        );
        await using var client = clientServices.BuildServiceProvider();
        var runtime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var transport = runtime.GetClientServerClientRuntime("cost");
            // The first rejection waits for real admission. Later operations have
            // a ready target and terminate at the existing message-bound owner,
            // before native submission. Both samples share that exact path.
            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await transport.SendAsync([Message.From(Body)], CancellationToken.None)
            );
            var stop = runtime.ShutdownToken;
            Compare(
                "ready send cancellation",
                () =>
                {
                    using var linked = CancellationTokenSource.CreateLinkedTokenSource(
                        CancellationToken.None,
                        stop
                    );
                    Reject(linked.Token);
                },
                () => Reject(CancellationToken.None)
            );

            void Reject(CancellationToken token)
            {
                var pending = transport.SendAsync([Message.From(Body)], token);
                Assert.True(pending.IsCompleted);
                try
                {
                    pending.GetAwaiter().GetResult();
                }
                catch (ZLinkFrameworkException error)
                    when (error.Kind == ZLinkFrameworkErrorKind.Rejected)
                {
                    return;
                }
                throw new InvalidOperationException(
                    "The ready target accepted an oversized message."
                );
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
        }
    }

    private void Compare(string name, Action baseline, Action optimized, Action? binding = null)
    {
        for (var index = 0; index < 1000; index++)
        {
            baseline();
            optimized();
            binding?.Invoke();
        }
        var a = new List<Sample>();
        var b = new List<Sample>();
        var native = new List<Sample>();
        for (var round = 0; round < 12; round++)
        {
            if ((round & 1) == 0)
            {
                Add(a, baseline);
                Add(b, optimized);
            }
            else
            {
                Add(b, optimized);
                Add(a, baseline);
            }
            if (binding is not null)
                Add(native, binding);
        }
        Assert.NotEmpty(a);
        Assert.NotEmpty(b);
        output.WriteLine(
            $"{name}: baseline {Median(a, true):F2} ns/op {Median(a, false):F2} B/op; optimized {Median(b, true):F2} ns/op {Median(b, false):F2} B/op; samples {a.Count}/{b.Count}"
        );
        if (binding is not null)
            output.WriteLine(
                $"binding body creation {Median(native, true):F2} ns/op {Median(native, false):F2} B/op; Framework/binding encode {Median(a, true) / Median(native, true):F3} -> {Median(b, true) / Median(native, true):F3}"
            );
        Assert.True(Median(b, false) < Median(a, false));

        static void Add(List<Sample> samples, Action action)
        {
            var gc0 = GC.CollectionCount(0);
            var gc1 = GC.CollectionCount(1);
            var gc2 = GC.CollectionCount(2);
            var before = GC.GetAllocatedBytesForCurrentThread();
            var started = Stopwatch.GetTimestamp();
            for (var index = 0; index < Iterations; index++)
                action();
            var elapsed = Stopwatch.GetElapsedTime(started).TotalNanoseconds;
            var allocated = GC.GetAllocatedBytesForCurrentThread() - before;
            if (
                gc0 == GC.CollectionCount(0)
                && gc1 == GC.CollectionCount(1)
                && gc2 == GC.CollectionCount(2)
            )
                samples.Add(new Sample(elapsed / Iterations, (double)allocated / Iterations));
        }
        static double Median(List<Sample> samples, bool elapsed) =>
            samples
                .Select(sample => elapsed ? sample.Nanoseconds : sample.Bytes)
                .Order()
                .ElementAt(samples.Count / 2);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static ValueTask<IReadOnlyList<Message>> BaselineRaw(int state) =>
        BaselineSubmitAsync(
            [],
            (_, _, _) => state == 42 ? Reply : throw new InvalidOperationException(),
            Budget,
            $"ClientServer request failed for '{state}': {{0}}.",
            CancellationToken.None
        );

    private static async ValueTask<IReadOnlyList<Message>> BaselineSubmitAsync(
        IReadOnlyList<Message> parts,
        Func<
            IReadOnlyList<Message>,
            TimeSpan,
            CancellationToken,
            Task<IReadOnlyList<Message>>
        > request,
        TimeSpan timeout,
        string failureMessage,
        CancellationToken cancellationToken
    )
    {
        ArgumentNullException.ThrowIfNull(parts);
        ArgumentNullException.ThrowIfNull(request);
        try
        {
            Task<IReadOnlyList<Message>> completion;
            try
            {
                completion = request(parts, timeout, cancellationToken);
            }
            catch (ZlinkSubmitException error)
            {
                throw ZLinkRequestFailureMapper.CreateSubmitException(
                    error,
                    string.Format(
                        System.Globalization.CultureInfo.InvariantCulture,
                        failureMessage,
                        error.Result
                    ),
                    completionFailure: false
                );
            }
            return await completion.ConfigureAwait(false);
        }
        catch (ZLinkRequestTerminalException terminal)
        {
            throw ZLinkRequestFailureMapper.CreateCompletionException(
                terminal.Result,
                terminal.FailureErrno,
                string.Format(
                    System.Globalization.CultureInfo.InvariantCulture,
                    failureMessage,
                    terminal.Result
                )
            );
        }
        catch (ZlinkRequestException error)
        {
            throw ZLinkRequestFailureMapper.CreateCompletionException(
                (RequestResult)(int)error.Result,
                string.Format(
                    System.Globalization.CultureInfo.InvariantCulture,
                    failureMessage,
                    error.Result
                )
            );
        }
        catch (ZlinkSubmitException error)
        {
            throw ZLinkRequestFailureMapper.CreateSubmitException(
                error,
                string.Format(
                    System.Globalization.CultureInfo.InvariantCulture,
                    failureMessage,
                    error.Result
                )
            );
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static ValueTask<IReadOnlyList<Message>> OptimizedRaw(int state) =>
        ZLinkRawRequestSubmitter.SubmitAsync(
            state,
            [],
            static (state, _, _, _) => state == 42 ? Reply : throw new InvalidOperationException(),
            Budget,
            static (_, _) =>
                throw new InvalidOperationException("A successful request formatted a failure."),
            CancellationToken.None
        );

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static async ValueTask BaselineWaitScope(bool ready)
    {
        if (ready)
            return;
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        void OnStateChanged() => changed.TrySetResult();
        GC.KeepAlive((Action)OnStateChanged);
        OnStateChanged();
        await changed.Task;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static async ValueTask ScopedWaitScope(bool ready)
    {
        if (ready)
            return;
        {
            var changed = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            void OnStateChanged() => changed.TrySetResult();
            GC.KeepAlive((Action)OnStateChanged);
            OnStateChanged();
            await changed.Task;
        }
    }

    private sealed class SelectionOwner : IDisposable
    {
        private readonly ZLinkStateLane _lane = new();
        private int _value;

        public void Dispose() => _lane.DisposeAsync().GetAwaiter().GetResult();

        [MethodImpl(MethodImplOptions.NoInlining)]
        internal int Bound() => _lane.RunAsync(() => ++_value).GetAwaiter().GetResult();

        [MethodImpl(MethodImplOptions.NoInlining)]
        internal int State() =>
            _lane.RunAsync(static owner => ++owner._value, this).GetAwaiter().GetResult();
    }

    private static (int Gen0, int Gen1, int Gen2) Collections() =>
        (GC.CollectionCount(0), GC.CollectionCount(1), GC.CollectionCount(2));

    private readonly record struct Sample(double Nanoseconds, double Bytes);

    private sealed class ProbeHandler : IZLinkSendHandler<Probe>
    {
        public ProbeHandler() { }

        public ValueTask HandleAsync(
            Probe message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    private sealed class Probe;

    private sealed class ProbeEnd : Exception;

    private sealed class PreambleSerializer : IZLinkMessageSerializer
    {
        internal long Started;
        internal long Allocated;

        public ZLinkEncodedPayload Serialize(object value, Type type)
        {
            Allocated = GC.GetAllocatedBytesForCurrentThread() - Started;
            throw new ProbeEnd();
        }

        public object? Deserialize(ZLinkEncodedPayload payload, Type type) =>
            throw new NotSupportedException();
    }

    private sealed class PartSerializer : IZLinkMessageSerializer, IZLinkMessagePartSerializer
    {
        public Message SerializePart(object value, Type type) => Message.From(Body);

        public ZLinkEncodedPayload Serialize(object value, Type type) =>
            ZLinkEncodedPayload.From(Body);

        public object? Deserialize(ZLinkEncodedPayload payload, Type type) => Payload;
    }

    private sealed class Activation(ZLinkMessageFlowTracer flow, ZLinkCodecRegistryBuilder codecs)
        : IZLinkCurrentSpotActivation
    {
        public void EnsureOperationAllowed() { }

        public string ChannelName => "work";
        public string SpotId => "work";
        public ZLinkUserSpotExecutionMode ExecutionMode => default;
        public TimeSpan DefaultRequestTimeout => Budget;
        public ZLinkCodecRegistryBuilder Codecs => codecs;
        public ZLinkMessageFlowTracer Flow => flow;
        public IZLinkRuntimeFailureReporter ErrorSink => null!;
        public IZLinkSpotOutbound Outbound => null!;
        public ZLinkSpotOutboundEndpoint OutboundEndpoint { get; set; } = null!;
    }
}
