using System.Collections.Concurrent;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class ShutdownHttpYieldContractTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Public_Http_Yield_Shutdown_And_Replacement_Instance(bool callbackFailure)
    {
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var server = new Zlink.HttpClient.UnitTests.TestHttpServer(async context =>
        {
            entered.TrySetResult();
            await release.Task;
            await context.Response.WriteAsync("late");
        });
        using var client = new System.Net.Http.HttpClient();
        var state = new ProbeState(client, server.BaseUrl, callbackFailure);
        var store = new ZLinkInMemoryProviderLocationStore();
        var address = "shutdown-http-" + Guid.NewGuid().ToString("N");
        using var host = Build(store, state);
        try
        {
            await host.StartAsync();
            var runtime = host.Services.GetRequiredService<IZLinkFrameworkRuntime>();
            await Ready(runtime);
            var call = host
                .Services.GetRequiredService<IZLinkSpotClient>()
                .RequestToSpot(address, new ProbeRequest(callbackFailure ? "echo" : "hold"))
                .InstanceSpot("probe")
                .InMesh("shutdown-http")
                .Async<ProbeReply>();
            var reply = call.AsTask();
            if (callbackFailure)
                Assert.Equal("echo", (await reply).Value);
            else
            {
                var arrival = await Task.WhenAny(entered.Task, reply);
                if (arrival == reply)
                    await reply;
                await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            }
            var result = await runtime.ShutdownAsync(
                callbackFailure ? TimeSpan.FromSeconds(2) : TimeSpan.FromMilliseconds(250)
            );
            Assert.Equal(ZLinkFrameworkTerminationOutcome.ForceStopped, result.Outcome);
            Assert.Equal(
                callbackFailure
                    ? ZLinkFrameworkTerminationReason.TeardownFailed
                    : ZLinkFrameworkTerminationReason.DeadlineExceeded,
                result.Reason
            );
            Assert.Equal(1, state.ClosingCalls);
            if (!callbackFailure)
            {
                Assert.Equal(1, state.Terminals);
                release.TrySetResult();
                Assert.Equal("late", await state.Original.Task.WaitAsync(TimeSpan.FromSeconds(5)));
                Assert.Equal(1, state.Terminals);
                state.Hold = false;
                using var replacement = Build(store, state);
                await replacement.StartAsync();
                await Ready(replacement.Services.GetRequiredService<IZLinkFrameworkRuntime>());
                Assert.Equal(
                    "echo",
                    (
                        await replacement
                            .Services.GetRequiredService<IZLinkSpotClient>()
                            .RequestToSpot(address, new ProbeRequest("echo"))
                            .InstanceSpot("probe")
                            .InMesh("shutdown-http")
                            .Async<ProbeReply>()
                    ).Value
                );
                await replacement.StopAsync();
                _ = await reply.ContinueWith(task => task.Exception);
                Assert.Contains(
                    state.Logs,
                    message => message.Contains("termination_changed", StringComparison.Ordinal)
                );
                Assert.DoesNotContain(
                    state.Logs,
                    message =>
                        message.Contains("reason=handler_exception", StringComparison.Ordinal)
                );
            }
        }
        finally
        {
            release.TrySetResult();
            await host.StopAsync();
        }
    }

    private static IHost Build(ZLinkInMemoryProviderLocationStore store, ProbeState state)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton(state);
        builder.Services.AddLogging(logging => logging.AddProvider(new CaptureLogs(state.Logs)));
        builder.Services.AddTransient<ProbeHandler>();
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddRelocationStore(new InMemoryRelocationStore());
            options.ConfigureLocations().PollingInterval = TimeSpan.FromMilliseconds(20);
            options
                .AddRouteMesh("shutdown-http")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server()
                .AddInstanceSpotFactory<ProbeSpot>("probe", factory => factory.DisableRelocation());
        });
        return builder.Build();
    }

    private static async Task Ready(IZLinkFrameworkRuntime runtime)
    {
        var end = DateTimeOffset.UtcNow.AddSeconds(5);
        while (!runtime.Status.IsReady)
        {
            if (DateTimeOffset.UtcNow >= end)
                throw new TimeoutException("host never became ready");
            await Task.Delay(2);
        }
    }

    private sealed class ProbeState(
        System.Net.Http.HttpClient client,
        string url,
        bool callbackFailure
    )
    {
        public System.Net.Http.HttpClient Client { get; } = client;
        public string Url { get; } = url;
        public bool CallbackFailure { get; } = callbackFailure;
        public bool Hold = true;
        public int Terminals;
        public int ClosingCalls;
        public ConcurrentQueue<string> Logs { get; } = new();
        public TaskCompletionSource<string> Original { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class CaptureLogs(ConcurrentQueue<string> records) : ILoggerProvider
    {
        public ILogger CreateLogger(string categoryName) => new CaptureLogger(records);

        public void Dispose() { }

        private sealed class CaptureLogger(ConcurrentQueue<string> records) : ILogger
        {
            public IDisposable? BeginScope<TState>(TState state)
                where TState : notnull => null;

            public bool IsEnabled(LogLevel level) => true;

            public void Log<TState>(
                LogLevel level,
                EventId id,
                TState state,
                Exception? error,
                Func<TState, Exception?, string> format
            ) => records.Enqueue(format(state, error));
        }
    }

    [ZLinkPacket("ShutdownProbe")]
    public sealed record ProbeRequest(string Value);

    public sealed record ProbeReply(string Value);

    private sealed class ProbeSpot(IZLinkInstanceSpotContext context, ProbeState state)
        : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;

        public void Configure() { }

        public ValueTask OnClosingAsync(
            ZLinkSpotClosingContext context,
            CancellationToken cancellationToken
        )
        {
            Interlocked.Increment(ref state.ClosingCalls);
            return state.CallbackFailure
                ? ValueTask.FromException(
                    new TimeoutException("application closing callback failed")
                )
                : ValueTask.CompletedTask;
        }
    }

    private sealed class ProbeHandler(ProbeState state)
        : IZLinkSpotRequestHandler<ProbeSpot, ProbeRequest, ProbeReply>
    {
        public async ValueTask<ProbeReply> HandleAsync(
            ProbeSpot spot,
            ProbeRequest request,
            CancellationToken cancellationToken
        )
        {
            if (request.Value != "hold" || !state.Hold)
                return new ProbeReply(request.Value);
            try
            {
                var result = await spot
                    .Context.RunIoWorker(async _ =>
                    {
                        var value = await state.Client.GetStringAsync(state.Url);
                        state.Original.TrySetResult(value);
                        return value;
                    })
                    .Yield();
                return new ProbeReply(result);
            }
            finally
            {
                Interlocked.Increment(ref state.Terminals);
            }
        }
    }
}
