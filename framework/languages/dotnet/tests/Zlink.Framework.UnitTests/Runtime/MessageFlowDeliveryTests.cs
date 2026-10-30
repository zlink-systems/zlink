using System.Diagnostics;
using System.Diagnostics.Metrics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class MessageFlowDeliveryTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Fact]
    public async Task NoProviderDetailedSkipsCaptureAndBuildersWithoutAllocating()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                typeof(MessageFlowDeliveryTests),
                nameof(NoProviderDetailedSkipsCaptureAndBuildersWithoutAllocating)
            )
        )
            return;

        var options = Options();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        AssertNoProviderAllocation(new ZLinkMessageFlowTracer(options));
    }

    [Fact]
    public async Task FrameworkRegisteredBeforeEmptyAddLoggingSkipsCaptureAndBuildersWithoutAllocating()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                typeof(MessageFlowDeliveryTests),
                nameof(
                    FrameworkRegisteredBeforeEmptyAddLoggingSkipsCaptureAndBuildersWithoutAllocating
                )
            )
        )
            return;

        var services = new ServiceCollection();
        services.AddZLinkFramework(options => options.DisableImplicitHandlerAutoRegistration());
        services.AddLogging();
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        runtime.Registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        Assert.False(runtime.Flow.CaptureEnabled);
        AssertNoProviderAllocation(runtime.Flow);
    }

    [Fact]
    public async Task RegisteredLoggerProviderDeliversAndProviderCheckDoesNotAllocate()
    {
        WaitForDelivery();
        var logger = new RecordingLogger();
        var services = new ServiceCollection();
        services.AddZLinkFramework(options => options.DisableImplicitHandlerAutoRegistration());
        services.AddLogging(builder => builder.AddProvider(new RecordingProvider(logger)));
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        runtime.Registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var configuredLogger = ZLinkMessageFlowTracer.GetLogger(provider);
        Assert.Same(configuredLogger, ZLinkMessageFlowTracer.GetLogger(provider));
        for (var i = 0; i < 10_000; ++i)
            Assert.True(ZLinkMessageFlowDelivery.HasProvider(configuredLogger));
        var present = 0;
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var i = 0; i < 10_000; ++i)
            if (ZLinkMessageFlowDelivery.HasProvider(configuredLogger))
                ++present;
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;
        output.WriteLine("provider-present allocated={0} checks={1}", allocated, present);
        Assert.Equal(10_000, present);
        Assert.Equal(0, allocated);

        var tracer = runtime.Flow;
        Assert.True(tracer.Enabled(ZLinkMessageFlowOutcome.Received));
        tracer.Trace(Event("registered-provider"));
        WaitForDelivery();
        Assert.Single(logger.Records);
        Assert.Contains(
            logger.Records[0].Fields,
            field => field.Key == "packet" && Equals(field.Value, "registered-provider")
        );
    }

    [Fact]
    public async Task LoggerFactoryCreateInstanceWithInternalProviderDeliversMessageFlow()
    {
        WaitForDelivery();
        var logger = new RecordingLogger();
        using var loggerFactory = LoggerFactory.Create(builder =>
            builder.AddProvider(new RecordingProvider(logger))
        );
        var services = new ServiceCollection();
        services.AddZLinkFramework(options => options.DisableImplicitHandlerAutoRegistration());
        services.AddLogging();
        services.AddSingleton<ILoggerFactory>(loggerFactory);
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        runtime.Registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);

        runtime.Flow.Trace(Event("factory-instance-provider"));
        WaitForDelivery();

        Assert.Contains(
            Assert.Single(logger.Records).Fields,
            field => field.Key == "packet" && Equals(field.Value, "factory-instance-provider")
        );
    }

    [Fact]
    public async Task ConcreteLoggerFactoryInstanceWithInternalProviderDeliversMessageFlow()
    {
        WaitForDelivery();
        var logger = new RecordingLogger();
        using var loggerFactory = new LoggerFactory([new RecordingProvider(logger)]);
        var services = new ServiceCollection();
        services.AddZLinkFramework(options => options.DisableImplicitHandlerAutoRegistration());
        services.AddLogging();
        services.AddSingleton<ILoggerFactory>(loggerFactory);
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        runtime.Registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);

        runtime.Flow.Trace(Event("concrete-factory-instance-provider"));
        WaitForDelivery();

        Assert.Contains(
            Assert.Single(logger.Records).Fields,
            field =>
                field.Key == "packet" && Equals(field.Value, "concrete-factory-instance-provider")
        );
    }

    [Fact]
    public async Task LastLoggerFactoryDescriptorControlsProviderDetection()
    {
        WaitForDelivery();
        var ignoredLogger = new RecordingLogger();
        using var earlierFactory = new LoggerFactory([new RecordingProvider(ignoredLogger)]);
        var services = new ServiceCollection();
        services.AddZLinkFramework(options => options.DisableImplicitHandlerAutoRegistration());
        services.AddSingleton<ILoggerFactory>(earlierFactory);
        services.AddLogging();
        services.AddSingleton<ILoggerFactory, LoggerFactory>();
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        runtime.Registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var tracer = runtime.Flow;

        Assert.False(tracer.CaptureEnabled);
        Assert.False(tracer.Enabled(ZLinkMessageFlowOutcome.Received));
        tracer.Trace(Event("shadowed-factory-instance"));
        WaitForDelivery();

        Assert.Empty(ignoredLogger.Records);
    }

    private void AssertNoProviderAllocation(ZLinkMessageFlowTracer tracer)
    {
        Assert.False(ZLinkTelemetry.ActivitySource.HasListeners());
        var captures = 0;
        var builders = 0;
        for (var i = 0; i < 10_000; ++i)
            Produce();
        captures = 0;
        builders = 0;
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var i = 0; i < 10_000; ++i)
            Produce();
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;
        output.WriteLine("allocated={0} captures={1} builders={2}", allocated, captures, builders);

        Assert.Equal(0, allocated);
        Assert.Equal(0, captures);
        Assert.Equal(0, builders);

        void Produce()
        {
            if (tracer.CaptureEnabled)
            {
                ++captures;
                using var scope = ZLinkFlowContext.EnterCurrentOrCreate(
                    ZLinkFlowOrigin.Application,
                    tracer.CaptureEnabled
                );
            }
            if (tracer.Enabled(ZLinkMessageFlowOutcome.Sent))
            {
                ++builders;
                tracer.Trace(Event("no-provider"));
            }
            if (tracer.Enabled(ZLinkMessageFlowOutcome.Dropped, ZLinkMessageFlowResult.Failed))
            {
                ++builders;
                tracer.Trace(Event("no-provider-error"));
            }
            if (tracer.DispatchErrorsEnabled)
                ++builders;
            if (tracer.DetailedEnabled)
                ++builders;
        }
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task BlockingLoggerDoesNotBlockProducer(bool blockIsEnabled)
    {
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var logger = new BlockingLogger(entered, release, blockIsEnabled);
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var tracer = new ZLinkMessageFlowTracer(options, logger);
        var producerThread = 0;
        var producer = Task.Run(() =>
        {
            producerThread = Environment.CurrentManagedThreadId;
            tracer.Trace(
                new ZLinkMessageFlowEvent(
                    ZLinkMessageFlowOutcome.Received,
                    ZLinkDispatchErrorSurface.Node,
                    ZLinkDispatchMessageKind.Send
                )
            );
        });
        try
        {
            Assert.True(entered.Wait(TimeSpan.FromSeconds(5)));
            await producer.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.NotEqual(producerThread, logger.ThreadId);
        }
        finally
        {
            release.Set();
            await producer.WaitAsync(TimeSpan.FromSeconds(5));
            WaitForDelivery();
        }
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task BlockingActivityCallbacksDoNotBlockProducer(bool started)
    {
        WaitForDelivery();
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var callbackThread = 0;
        void Block(Activity activity)
        {
            callbackThread = Environment.CurrentManagedThreadId;
            entered.Set();
            release.Wait();
        }
        var sourceReference = ZLinkTelemetry.ActivitySource;
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => ReferenceEquals(source, sourceReference),
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStarted = started ? Block : null,
            ActivityStopped = started ? null : Block,
        };
        ActivitySource.AddActivityListener(listener);
        var options = Options();
        var tracer = new ZLinkMessageFlowTracer(options);
        var producerThread = 0;
        var producer = Task.Run(() =>
        {
            producerThread = Environment.CurrentManagedThreadId;
            tracer.Trace(Event("activity"));
        });
        try
        {
            Assert.True(entered.Wait(TimeSpan.FromSeconds(5)));
            await producer.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.NotEqual(producerThread, callbackThread);
        }
        finally
        {
            release.Set();
            await producer.WaitAsync(TimeSpan.FromSeconds(5));
            WaitForDelivery();
        }
    }

    [Fact]
    public void CapacityExcludesExecutingRecordAndDropsAllNewRecordKinds()
    {
        WaitForDelivery();
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var blocker = new ZLinkMessageFlowTracer(
            Options(),
            new BlockingLogger(entered, release, false)
        );
        var capture = new RecordingLogger();
        var tracer = new ZLinkMessageFlowTracer(Options(), capture);
        var owner = ZLinkMessageFlowDelivery.Shared;
        var before = owner.State;
        long metric = 0;
        Instrument? published = null;
        var metricThread = 0;
        using var meter = new MeterListener();
        meter.InstrumentPublished = (instrument, listener) =>
        {
            if (instrument.Name == "zlink.observability.events.overflow")
            {
                published = instrument;
                listener.EnableMeasurementEvents(instrument);
            }
        };
        meter.SetMeasurementEventCallback<long>(
            (_, value, tags, _) =>
            {
                foreach (var tag in tags)
                    if (tag.Key == "source" && Equals(tag.Value, "message_flow"))
                    {
                        Interlocked.Add(ref metric, value);
                        metricThread = Environment.CurrentManagedThreadId;
                    }
            }
        );
        meter.Start();
        try
        {
            blocker.Trace(Event("executing"));
            Assert.True(entered.Wait(TimeSpan.FromSeconds(5)));
            for (var index = 0; index < ZLinkMessageFlowDelivery.Capacity; ++index)
                tracer.Trace(Event(index.ToString()));
            var full = owner.State;
            Assert.Equal(ZLinkMessageFlowDelivery.Capacity, full.Pending);
            Assert.True(full.Executing);
            tracer.Trace(Event("overflow-normal"));
            tracer.TraceDispatchError(Failure("overflow-error"));
            tracer.Trace(Event("overflow-drop") with { Outcome = ZLinkMessageFlowOutcome.Dropped });
            Assert.Equal(before.Overflow + 3, owner.State.Overflow);
            Assert.Equal(before.Wakes + 1, owner.State.Wakes);
            Assert.Equal(0, Interlocked.Read(ref metric));
        }
        finally
        {
            release.Set();
            try
            {
                WaitForDelivery();
            }
            finally
            {
                if (published is not null)
                    meter.DisableMeasurementEvents(published);
            }
        }
        Assert.Equal(3, Interlocked.Read(ref metric));
        Assert.NotEqual(Environment.CurrentManagedThreadId, metricThread);
        Assert.Equal(
            Enumerable
                .Range(0, ZLinkMessageFlowDelivery.Capacity)
                .Select(value => value.ToString()),
            capture.Records.Select(record =>
                (string)record.Fields.Single(field => field.Key == "packet").Value!
            )
        );
        Assert.Single(capture.Records.Select(record => record.Thread).Distinct());
    }

    [Fact]
    public void HostRetirementDiscardsPendingAndRenewalNeverRevivesOldLease()
    {
        WaitForDelivery();
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        using var hostA = new ZLinkTelemetrySubmission();
        using var hostB = new ZLinkTelemetrySubmission();
        var blockingLogger = new BlockingLogger(entered, release, false);
        var active = new ZLinkMessageFlowTracer(Options(), blockingLogger, submission: hostA);
        var captured = new RecordingLogger();
        var a = new ZLinkMessageFlowTracer(Options(), captured, submission: hostA);
        var b = new ZLinkMessageFlowTracer(Options(), captured, submission: hostB);
        var before = ZLinkMessageFlowDelivery.Shared.State.Overflow;
        try
        {
            active.Trace(Event("active"));
            Assert.True(entered.Wait(TimeSpan.FromSeconds(5)));
            a.Trace(Event("retired"));
            b.Trace(Event("other-host"));
            var old = hostA.Current;
            hostA.Dispose();
            Assert.True(old.Retired);
            Assert.Equal(1, ZLinkMessageFlowDelivery.Shared.State.Pending);
            a.Trace(Event("closed"));
            hostA.Renew();
            Assert.NotSame(old, hostA.Current);
            a.Trace(Event("new-generation"));
            Assert.True(old.Retired);
            Assert.Equal(2, ZLinkMessageFlowDelivery.Shared.State.Pending);
        }
        finally
        {
            release.Set();
            WaitForDelivery();
        }
        Assert.Equal(before, ZLinkMessageFlowDelivery.Shared.State.Overflow);
        Assert.Equal(
            new[] { "other-host", "new-generation" },
            captured.Records.Select(record =>
                (string)record.Fields.Single(field => field.Key == "packet").Value!
            )
        );
        Assert.All(
            captured.Records,
            record => Assert.Equal(blockingLogger.ThreadId, record.Thread)
        );
    }

    [Fact]
    public void QueuedRecordsFreezeProcessingLevelAndSanitizedErrorData()
    {
        WaitForDelivery();
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var blocker = new ZLinkMessageFlowTracer(
            Options(),
            new BlockingLogger(entered, release, false)
        );
        var options = Options();
        var captured = new RecordingLogger();
        var tracer = new ZLinkMessageFlowTracer(options, captured);
        try
        {
            blocker.Trace(Event("block"));
            Assert.True(entered.Wait(TimeSpan.FromSeconds(5)));
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Errors);
            tracer.TraceDispatchError(
                Failure("errors") with
                {
                    FlowId = "explicit",
                    FlowOrigin = ZLinkFlowOrigin.Application,
                    Exception = new InvalidOperationException("failure token=private\nsecond line"),
                }
            );
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            tracer.Trace(
                Event("normal") with
                {
                    FlowId = "captured",
                    FlowOrigin = ZLinkFlowOrigin.Application,
                }
            );
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Errors);
        }
        finally
        {
            release.Set();
            WaitForDelivery();
        }
        var errors = captured.Records[0].Fields;
        Assert.DoesNotContain(errors, field => field.Key is "flow" or "origin");
        Assert.Contains(
            errors,
            field => field.Key == "error_message" && Equals(field.Value, "failure token=<redacted>")
        );
        Assert.Contains(
            captured.Records[1].Fields,
            field => field.Key == "flow" && Equals(field.Value, "captured")
        );
    }

    [Fact]
    public async Task PublicRequestAndHostStopCompleteWhileDispatchLoggerIsBlocked()
    {
        WaitForDelivery();
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var logger = new BlockingLogger(entered, release, true);
        var services = new ServiceCollection();
        services.AddSingleton<ILoggerFactory>(new FlowLoggerFactory(logger));
        services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options
                .AddClientServerChannel("delivery-proof")
                .Server()
                .Listen(0)
                .AddRequestHandler<ProofHandler, ProofRequest, ProofReply>();
        });
        await using var server = services.BuildServiceProvider();
        var runtime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        server
            .GetRequiredService<ZLinkFrameworkRegistration>()
            .DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        await runtime.StartAsync(CancellationToken.None);
        var cachedFlow = runtime.Flow;
        var originalLease = runtime.TelemetrySubmission.Current;
        var state = await runtime.GetStartedStateForRoutingAsync(CancellationToken.None);
        var endpoint = (
            await state.ClientServerServerBundles.Values.Single().ClientServerServer!.ReadAsync()
        ).AdvertisedEndpoint;
        var clientServices = new ServiceCollection();
        clientServices.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.AddClientServerChannel("delivery-proof").Client().Connect(endpoint);
        });
        await using var client = clientServices.BuildServiceProvider();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            var deadline = Stopwatch.StartNew();
            while (clientRuntime.GetClientServerClientRuntime("delivery-proof").ReadyCount != 1)
            {
                Assert.True(deadline.Elapsed < TimeSpan.FromSeconds(10));
                await Task.Delay(10);
            }
            var request = client
                .GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("delivery-proof", new ProofRequest("owned"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<ProofReply>()
                .AsTask();
            Assert.True(entered.Wait(TimeSpan.FromSeconds(5)));
            var reply = await request.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal("owned", reply.Value);
            foreach (ProcessModule module in Process.GetCurrentProcess().Modules)
                if (module.ModuleName.StartsWith("libzlink", StringComparison.Ordinal))
                    output.WriteLine(
                        "loaded-native={0} sha256={1}",
                        module.FileName,
                        Convert.ToHexString(
                            System.Security.Cryptography.SHA256.HashData(
                                File.ReadAllBytes(module.FileName)
                            )
                        )
                    );
            await runtime
                .StopAsync(CancellationToken.None)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));
            Assert.True(server.GetRequiredService<ZLinkTelemetrySubmission>().Current.Retired);
            await runtime.StartAsync(CancellationToken.None);
            Assert.Same(cachedFlow, runtime.Flow);
            Assert.NotSame(originalLease, runtime.TelemetrySubmission.Current);
            Assert.True(originalLease.Retired);
            cachedFlow.Trace(Event("restarted-generation"));
            Assert.Equal(1, ZLinkMessageFlowDelivery.Shared.State.Pending);
            await runtime.StopAsync(CancellationToken.None);
        }
        finally
        {
            release.Set();
            WaitForDelivery();
            await clientRuntime.StopAsync(CancellationToken.None);
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public void ProviderFailuresAreRoleCountedAndFailedFallbackIsNeverRetried()
    {
        WaitForDelivery();
        var owner = ZLinkMessageFlowDelivery.Shared;
        var loggerBefore = owner.FailureCount(ZLinkTelemetryProviderRole.Logger);
        var traceBefore = owner.FailureCount(ZLinkTelemetryProviderRole.Trace);
        var fallbackBefore = owner.FailureCount(ZLinkTelemetryProviderRole.Fallback);
        var previous = Console.Error;
        var fallback = new FailingFallbackWriter();
        var sourceReference = ZLinkTelemetry.ActivitySource;
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => ReferenceEquals(source, sourceReference),
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                throw new IOException("trace provider"),
        };
        ActivitySource.AddActivityListener(listener);
        try
        {
            Console.SetError(fallback);
            var services = new ServiceCollection();
            services.AddSingleton<ILoggerFactory>(new FlowLoggerFactory(new ThrowingLogger()));
            services.AddZLinkFramework(options => options.DisableImplicitHandlerAutoRegistration());
            using var provider = services.BuildServiceProvider();
            var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
            provider
                .GetRequiredService<ZLinkFrameworkRegistration>()
                .DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            var callbacks = 0;
            runtime.ErrorSink.UnhandledCallbackException += _ => ++callbacks;
            var tracer = runtime.Flow;
            for (var index = 0; index < 3; ++index)
                tracer.Trace(Event("failure"));
            WaitForDelivery();
            Assert.Equal(loggerBefore + 3, owner.FailureCount(ZLinkTelemetryProviderRole.Logger));
            Assert.Equal(traceBefore + 3, owner.FailureCount(ZLinkTelemetryProviderRole.Trace));
            Assert.Equal(
                fallbackBefore + 1,
                owner.FailureCount(ZLinkTelemetryProviderRole.Fallback)
            );
            Assert.Equal(2, fallback.Attempts);
            Assert.Equal(0, callbacks);
            listener.Dispose();
            using var entered = new ManualResetEventSlim();
            using var release = new ManualResetEventSlim();
            using var meterEntered = new ManualResetEventSlim();
            using var meterRelease = new ManualResetEventSlim();
            using var submission = new ZLinkTelemetrySubmission();
            var blocked = new ZLinkMessageFlowTracer(
                Options(),
                new BlockingLogger(entered, release, false),
                submission: submission
            );
            var accepted = new RecordingLogger();
            var queued = new ZLinkMessageFlowTracer(Options(), accepted, submission: submission);
            var metricThread = 0;
            Instrument? overflowInstrument = null;
            using var meter = new MeterListener();
            meter.InstrumentPublished = (instrument, consumer) =>
            {
                if (instrument.Name == "zlink.observability.events.overflow")
                {
                    overflowInstrument = instrument;
                    consumer.EnableMeasurementEvents(instrument);
                }
            };
            meter.SetMeasurementEventCallback<long>(
                (_, _, tags, _) =>
                {
                    foreach (var tag in tags)
                        if (tag.Key == "source" && Equals(tag.Value, "message_flow"))
                        {
                            metricThread = Environment.CurrentManagedThreadId;
                            meterEntered.Set();
                            meterRelease.Wait();
                            throw new IOException("overflow metric provider");
                        }
                }
            );
            meter.Start();
            try
            {
                blocked.Trace(Event("metric-block"));
                Assert.True(entered.Wait(TimeSpan.FromSeconds(5)));
                for (var index = 0; index < ZLinkMessageFlowDelivery.Capacity; ++index)
                    queued.Trace(Event("queued"));
                queued.Trace(Event("overflow"));
                release.Set();
                Assert.True(meterEntered.Wait(TimeSpan.FromSeconds(5)));
                Assert.NotEqual(Environment.CurrentManagedThreadId, metricThread);
                // Retiring a host and submitting another host's record do not wait for Counter.Add.
                submission.Dispose();
                var otherHost = new ZLinkMessageFlowTracer(Options(), accepted);
                otherHost.Trace(Event("accepted-while-meter-blocked"));
                Assert.Equal(1, owner.State.Pending);
                Assert.Equal(2, fallback.Attempts);
            }
            finally
            {
                release.Set();
                meterRelease.Set();
                try
                {
                    WaitForDelivery();
                }
                finally
                {
                    if (overflowInstrument is not null)
                        meter.DisableMeasurementEvents(overflowInstrument);
                }
            }
            Assert.Equal(traceBefore + 4, owner.FailureCount(ZLinkTelemetryProviderRole.Trace));
            Assert.Equal(
                fallbackBefore + 1,
                owner.FailureCount(ZLinkTelemetryProviderRole.Fallback)
            );
            Assert.Equal(2, fallback.Attempts);
            Assert.Equal(0, callbacks);
        }
        finally
        {
            Console.SetError(previous);
        }
    }

    private sealed class FailingFallbackWriter : StringWriter
    {
        internal int Attempts;

        public override void WriteLine(string? value)
        {
            if (++Attempts == 2)
                throw new IOException("fallback provider");
            base.WriteLine(value);
        }
    }

    private sealed class ThrowingLogger : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel level) => throw new IOException("logger provider");

        public void Log<TState>(
            LogLevel level,
            EventId id,
            TState state,
            Exception? error,
            Func<TState, Exception?, string> formatter
        ) => throw new InvalidOperationException();
    }

    private sealed class FlowLoggerFactory(ILogger logger) : ILoggerFactory
    {
        public ILogger CreateLogger(string category) =>
            category == ZLinkMessageFlowTracer.LoggerCategory ? logger : NullLogger.Instance;

        public void AddProvider(ILoggerProvider provider) { }

        public void Dispose() { }
    }

    private sealed record ProofRequest(string Value);

    private sealed record ProofReply(string Value);

    private sealed class ProofHandler : IZLinkRequestHandler<ProofRequest, ProofReply>
    {
        public ValueTask<ProofReply> HandleAsync(
            ProofRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(new ProofReply(request.Value));
    }

    [Fact]
    public void ParentActivityAndFlowAreFrozenWithoutWorkerAmbientState()
    {
        WaitForDelivery();
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var blocker = new ZLinkMessageFlowTracer(
            Options(),
            new BlockingLogger(entered, release, false)
        );
        var logger = new RecordingLogger();
        var tracer = new ZLinkMessageFlowTracer(Options(), logger);
        var activities = new List<Activity>();
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activities.Add,
        };
        ActivitySource.AddActivityListener(listener);
        ActivityContext parent;
        string? flow;
        try
        {
            blocker.Trace(Event("block"));
            Assert.True(entered.Wait(TimeSpan.FromSeconds(5)));
            using (
                var activity = new Activity("producer").SetIdFormat(ActivityIdFormat.W3C).Start()
            )
            using (ZLinkFlowContext.Enter(null, null, true, ZLinkFlowOrigin.Inbound))
            {
                parent = activity.Context;
                flow = ZLinkFlowContext.Current!.Value.FlowId;
                tracer.Trace(Event("frozen-parent"));
            }
        }
        finally
        {
            release.Set();
            WaitForDelivery();
        }
        var delivered = Assert.Single(
            activities.Where(activity =>
                Equals(activity.GetTagItem("packet_name"), "frozen-parent")
            )
        );
        Assert.Equal(parent.TraceId, delivered.TraceId);
        Assert.Equal(parent.SpanId, delivered.ParentSpanId);
        Assert.Contains(
            logger.Records.Single().Fields,
            field => field.Key == "flow" && Equals(field.Value, flow)
        );
        Assert.Null(logger.AmbientFlow);
    }

    [Fact]
    public void NoProvidersSkipCallSiteEventsAndQueueAllocation()
    {
        WaitForDelivery();
        var tracer = new ZLinkMessageFlowTracer(Options());
        Assert.False(ZLinkTelemetry.ActivitySource.HasListeners());
        var before = ZLinkMessageFlowDelivery.Shared.State;
        _ = tracer.Enabled(ZLinkMessageFlowOutcome.Received);
        _ = tracer.DispatchErrorsEnabled;
        var allocations = GC.GetAllocatedBytesForCurrentThread();
        var constructed = 0;
        for (var index = 0; index < 100; ++index)
        {
            if (tracer.Enabled(ZLinkMessageFlowOutcome.Received))
            {
                ++constructed;
                tracer.Trace(Event("not-constructed"));
            }
            if (tracer.DispatchErrorsEnabled)
            {
                ++constructed;
                tracer.TraceDispatchError(Failure("not-constructed"));
            }
        }
        var allocated = GC.GetAllocatedBytesForCurrentThread() - allocations;
        Assert.Equal(0, constructed);
        Assert.Equal(0, allocated);
        Assert.Equal(before, ZLinkMessageFlowDelivery.Shared.State);
    }

    private static ZLinkDispatchOptionsModel Options()
    {
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        return options;
    }

    private static ZLinkMessageFlowEvent Event(string packet) =>
        new(
            ZLinkMessageFlowOutcome.Received,
            ZLinkDispatchErrorSurface.Node,
            ZLinkDispatchMessageKind.Send,
            PacketName: packet
        );

    private static ZLinkDispatchFailure Failure(string packet) =>
        new(
            ZLinkDispatchErrorSurface.Node,
            ZLinkDispatchMessageKind.Send,
            ZLinkDispatchErrorReason.HandlerException,
            ZLinkDispatchErrorAction.Drop,
            packet
        );

    internal static void WaitForDelivery() =>
        Assert.True(
            SpinWait.SpinUntil(
                () => ZLinkMessageFlowDelivery.Shared.State is { Pending: 0, Executing: false },
                TimeSpan.FromSeconds(10)
            )
        );

    private sealed class RecordingProvider(ILogger logger) : ILoggerProvider
    {
        public ILogger CreateLogger(string categoryName) => logger;

        public void Dispose() { }
    }

    private sealed class RecordingLogger : ILogger
    {
        internal readonly List<(int Thread, KeyValuePair<string, object?>[] Fields)> Records = [];
        internal string? AmbientFlow;

        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel level) => true;

        public void Log<TState>(
            LogLevel level,
            EventId id,
            TState state,
            Exception? error,
            Func<TState, Exception?, string> formatter
        )
        {
            AmbientFlow = ZLinkFlowContext.Current?.FlowId;
            Records.Add(
                (
                    Environment.CurrentManagedThreadId,
                    ((IEnumerable<KeyValuePair<string, object?>>)(object)state!).ToArray()
                )
            );
        }
    }

    private sealed class BlockingLogger(
        ManualResetEventSlim entered,
        ManualResetEventSlim release,
        bool blockIsEnabled
    ) : ILogger
    {
        public int ThreadId;

        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel level)
        {
            if (blockIsEnabled)
                Block();
            return true;
        }

        public void Log<TState>(
            LogLevel level,
            EventId id,
            TState state,
            Exception? error,
            Func<TState, Exception?, string> formatter
        )
        {
            if (!blockIsEnabled)
                Block();
        }

        private void Block()
        {
            ThreadId = Environment.CurrentManagedThreadId;
            entered.Set();
            release.Wait();
        }
    }
}

// A dedicated child testhost makes the absence assertion independent of suite ordering.
public sealed class MessageFlowDeliveryAbsenceTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Fact]
    public async Task HostWithoutProvidersDoesNotCreateDeliveryOwner()
    {
        const string childMarker = "ZLINK_TELEMETRY_ABSENCE_CHILD";
        if (Environment.GetEnvironmentVariable(childMarker) != "1")
        {
            var start = new ProcessStartInfo("dotnet")
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            start.ArgumentList.Add("vstest");
            start.ArgumentList.Add(typeof(MessageFlowDeliveryAbsenceTests).Assembly.Location);
            start.ArgumentList.Add(
                "/TestCaseFilter:FullyQualifiedName~MessageFlowDeliveryAbsenceTests"
            );
            start.Environment[childMarker] = "1";
            using var child = Process.Start(start)!;
            output.WriteLine(
                "child-pid={0} argv=dotnet {1}",
                child.Id,
                string.Join(" ", start.ArgumentList)
            );
            var assembly = typeof(MessageFlowDeliveryAbsenceTests).Assembly.Location;
            output.WriteLine(
                "test-assembly={0} sha256={1}",
                assembly,
                Convert.ToHexString(
                    System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(assembly))
                )
            );
            var stdout = child.StandardOutput.ReadToEndAsync();
            var stderr = child.StandardError.ReadToEndAsync();
            try
            {
                await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(30));
                output.WriteLine(await stdout);
                output.WriteLine(await stderr);
                Assert.Equal(0, child.ExitCode);
                Assert.Contains("Passed:     1", await stdout);
            }
            finally
            {
                if (!child.HasExited)
                {
                    child.Kill(entireProcessTree: true);
                    await child.WaitForExitAsync();
                }
            }
            return;
        }
        Assert.False(ZLinkMessageFlowDelivery.IsCreated);
        var services = new ServiceCollection();
        services.AddZLinkFramework(options => options.DisableImplicitHandlerAutoRegistration());
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        provider.GetRequiredService<IZLinkDiagnosticsRuntime>().Level =
            ZLinkDiagnosticsLevel.Normal;
        Assert.False(runtime.Flow.CaptureEnabled);
        Assert.Equal(
            ZLinkDiagnosticsLevel.Normal,
            provider.GetRequiredService<IZLinkDiagnosticsRuntime>().Level
        );
        Assert.False(ZLinkTelemetry.ActivitySource.HasListeners());
        await runtime.StopAsync(CancellationToken.None);
        Assert.True(runtime.TelemetrySubmission.Current.Retired);
        Assert.False(ZLinkMessageFlowDelivery.IsCreated);
        await runtime.StartAsync(CancellationToken.None);
        await runtime.StopAsync(CancellationToken.None);
        Assert.False(ZLinkMessageFlowDelivery.IsCreated);
        provider.GetRequiredService<ZLinkTelemetrySubmission>().Dispose();
        Assert.False(ZLinkMessageFlowDelivery.IsCreated);
    }
}

internal static class TelemetryReceipt
{
    internal static void Wait(Func<bool> received) =>
        Assert.True(
            SpinWait.SpinUntil(received, TimeSpan.FromSeconds(5)),
            "The expected telemetry provider receipt was not observed."
        );
}
