using System.Net;
using System.Net.Sockets;
using System.Threading.Tasks.Sources;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class BlockingSubmitTests
{
    [Theory]
    [InlineData("handler")]
    [InlineData("spot")]
    [InlineData("state-lane")]
    public async Task RuntimeContext_RejectsBeforeStartingCall_AndRestoresApplicationContext(string context)
    {
        var send = new SendProbe(ValueTask.CompletedTask);
        var request = new RequestProbe(ValueTask.FromResult(42));

        switch (context)
        {
            case "handler":
                using (var queue = new ZLinkApplicationJobQueue(new(
                           ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1)))
                using (var lease = await queue.AcquireAsync(CancellationToken.None))
                using (ZLinkApplicationJobQueueInvocation.Enter(lease))
                {
                    await ZLinkHandlerInvocationEngine.InvokeAsync(
                        new object(),
                        (_, _, _, _, _, _) => CheckHandlerAsync());

                    async Task CheckHandlerAsync()
                    {
                        Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
                        Assert.Null(ZLinkSpotAmbientContext.CurrentOrDefault);
                        Assert.Null(ZLinkStateLane.Current);
                        AssertBlocked(send, request);
                        await Task.Yield();
                        AssertBlocked(send, request);
                    }
                }
                break;
            case "spot":
                using (var errors = new ZLinkRuntimeErrorSink())
                await using (var queue = new ZLinkSerialExecutionQueue(
                                 new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
                                 errors, CancellationToken.None))
                {
                    var work = await queue.PostAsync(async _ =>
                    {
                        using var activation = ZLinkSpotAmbientContext.Push(new SpotActivation());
                        Assert.NotNull(ZLinkSerialTurn.Current);
                        Assert.False(ZLinkApplicationJobQueueInvocation.IsActive);
                        Assert.Null(ZLinkStateLane.Current);
                        AssertBlocked(send, request);
                        await Task.Yield();
                        AssertBlocked(send, request);
                    }, CancellationToken.None);
                    await work.Completion.WaitAsync(TimeSpan.FromSeconds(5));
                }
                break;
            case "state-lane":
                await using (var lane = new ZLinkStateLane())
                {
                    await lane.RunAsync(async () =>
                    {
                        Assert.False(ZLinkApplicationJobQueueInvocation.IsActive);
                        Assert.Null(ZLinkSpotAmbientContext.CurrentOrDefault);
                        AssertBlocked(send, request);
                        await Task.Yield();
                        AssertBlocked(send, request);
                    });
                }
                break;
        }

        ((IZLinkSendCall)send).Submit();
        Assert.Equal(42, ((IZLinkRequestCall)request).Submit<int>());
        Assert.Equal(1, send.Submissions);
        Assert.Equal(1, request.Submissions);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ApplicationThread_BlocksUntilPendingValueTaskSourceCompletes(bool isRequest)
    {
        var source = new PendingResult();
        var send = new SendProbe(source.SendResult);
        var request = new RequestProbe(source.RequestResult);
        var returned = new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
        var caller = new Thread(() =>
        {
            try
            {
                var result = 0;
                if (isRequest)
                    result = ((IZLinkRequestCall)request).Submit<int>();
                else
                    ((IZLinkSendCall)send).Submit();
                returned.SetResult(result);
            }
            catch (Exception error)
            {
                returned.SetException(error);
            }
        }) { IsBackground = true };

        caller.Start();
        try
        {
            await source.WaitRegistered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(returned.Task.IsCompleted);
        }
        finally
        {
            source.Complete(42);
        }

        Assert.Equal(isRequest ? 42 : 0,
            await returned.Task.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.True(caller.Join(TimeSpan.FromSeconds(5)));
        Assert.Equal(1, source.ResultReads);
        Assert.Equal(caller.ManagedThreadId,
            isRequest ? request.SubmittingThreadId : send.SubmittingThreadId);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void ApplicationThread_PropagatesTerminalFailureWithoutWrapping(bool isRequest)
    {
        var failure = new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, "unavailable");
        IZLinkSendCall send = new SendProbe(ValueTask.FromException(failure));
        IZLinkRequestCall request = new RequestProbe(ValueTask.FromException<int>(failure));
        var observed = Assert.Throws<ZLinkFrameworkException>(() =>
        {
            if (isRequest)
                request.Submit<int>();
            else
                send.Submit();
        });
        Assert.Same(failure, observed);
    }

    [Fact]
    public async Task ApplicationThread_SendReturnsAfterAdmissionWhileHandlerStillRuns()
    {
        await using var peers = await Peers.StartAsync();
        try
        {
            peers.Client.SendToChannel("work", new SendMessage("admitted")).Submit();
            Assert.Equal("admitted",
                await peers.Probe.SendEntered.Task.WaitAsync(TimeSpan.FromSeconds(5)));
            Assert.False(peers.Probe.SendCompleted.Task.IsCompleted);
        }
        finally
        {
            peers.Probe.ReleaseSend.TrySetResult();
        }
        await peers.Probe.SendCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task ApplicationThread_RequestReturnsTypedReply()
    {
        await using var peers = await Peers.StartAsync();
        var reply = peers.Client.RequestToChannel("work", new RequestMessage("reply"))
            .Submit<ReplyMessage>();
        Assert.Equal("reply", reply.Value);
    }

    [Fact]
    public async Task DispatchedHandler_RejectsBlockingCallsBeforeSubmission()
    {
        await using var peers = await Peers.StartAsync();
        var send = peers.Client.SendToChannel("work", new SendMessage("after-handler"));
        var request = peers.Client.RequestToChannel("work", new RequestMessage("after-handler"));
        var checks = 0;
        peers.Probe.InHandler = () =>
        {
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation,
                Assert.Throws<ZLinkFrameworkException>(() => send.Submit()).Kind);
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation,
                Assert.Throws<ZLinkFrameworkException>(() => request.Submit<ReplyMessage>()).Kind);
            Interlocked.Increment(ref checks);
        };

        var reply = await peers.Client.RequestToChannel("work", new RequestMessage("in-handler"))
            .Async<ReplyMessage>();

        Assert.Equal("in-handler", reply.Value);
        Assert.Equal(2, checks);
        Assert.False(peers.Probe.SendEntered.Task.IsCompleted);
        Assert.Equal(1, peers.Probe.Requests);
        peers.Probe.InHandler = null;
        // Reusing rejected builders proves that rejection did not claim admission.
        send.Submit();
        peers.Probe.ReleaseSend.TrySetResult();
        Assert.Equal("after-handler", request.Submit<ReplyMessage>().Value);
        Assert.Equal(2, peers.Probe.Requests);
    }

    private static void AssertBlocked(SendProbe send, RequestProbe request)
    {
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation,
            Assert.Throws<ZLinkFrameworkException>(() => ((IZLinkSendCall)send).Submit()).Kind);
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation,
            Assert.Throws<ZLinkFrameworkException>(() => ((IZLinkRequestCall)request).Submit<int>()).Kind);
        Assert.Equal(0, send.Submissions);
        Assert.Equal(0, request.Submissions);
    }

    private sealed class SendProbe(ValueTask result) : IZLinkSendCall
    {
        public int Submissions { get; private set; }
        public int SubmittingThreadId { get; private set; }
        public IZLinkSendCall Metadata(string key, string value) => this;
        public IZLinkSendCall Metadata(ZLinkMessageMetadata metadata) => this;
        public ValueTask Async(CancellationToken cancellationToken = default)
        {
            Submissions++;
            SubmittingThreadId = Environment.CurrentManagedThreadId;
            return result;
        }
    }

    private sealed class RequestProbe(ValueTask<int> result) : IZLinkRequestCall
    {
        public int Submissions { get; private set; }
        public int SubmittingThreadId { get; private set; }
        public IZLinkRequestCall Metadata(string key, string value) => this;
        public IZLinkRequestCall Metadata(ZLinkMessageMetadata metadata) => this;
        public IZLinkRequestCall Timeout(TimeSpan timeout) => this;
        public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default)
        {
            Submissions++;
            SubmittingThreadId = Environment.CurrentManagedThreadId;
            return (ValueTask<TReply>)(object)result;
        }
        public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();
    }

    private sealed class PendingResult : IValueTaskSource, IValueTaskSource<int>
    {
        private ManualResetValueTaskSourceCore<int> _source = new() { RunContinuationsAsynchronously = true };
        public TaskCompletionSource WaitRegistered { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public ValueTask SendResult => new(this, _source.Version);
        public ValueTask<int> RequestResult => new(this, _source.Version);
        public int ResultReads { get; private set; }
        public void Complete(int value) => _source.SetResult(value);
        public ValueTaskSourceStatus GetStatus(short token) => _source.GetStatus(token);
        public int GetResult(short token)
        {
            ResultReads++;
            return _source.GetResult(token);
        }
        void IValueTaskSource.GetResult(short token) => GetResult(token);
        public void OnCompleted(Action<object?> continuation, object? state, short token,
            ValueTaskSourceOnCompletedFlags flags)
        {
            _source.OnCompleted(continuation, state, token, flags);
            WaitRegistered.SetResult();
        }
    }

    private sealed class SpotActivation : IZLinkCurrentSpotActivation
    {
        public void EnsureOperationAllowed() { }
        public string ChannelName => "work";
        public string SpotId => "spot";
        public ZLinkUserSpotExecutionMode ExecutionMode => ZLinkUserSpotExecutionMode.SpotWide;
        public TimeSpan DefaultRequestTimeout => TimeSpan.FromSeconds(1);
        public ZLinkCodecRegistryBuilder Codecs => null!;
        public ZLinkMessageFlowTracer Flow => null!;
        public IZLinkRuntimeFailureReporter ErrorSink => null!;
        public IZLinkSpotOutbound Outbound => null!;
        public ZLinkSpotOutboundEndpoint OutboundEndpoint => null!;
    }

    private sealed record SendMessage(string Value);
    private sealed record RequestMessage(string Value);
    private sealed record ReplyMessage(string Value);

    private sealed class HandlerProbe
    {
        public TaskCompletionSource<string> SendEntered { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseSend { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource SendCompleted { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Action? InHandler { get; set; }
        public int Requests;
    }

    private sealed class SendHandler(HandlerProbe probe) : IZLinkSendHandler<SendMessage>
    {
        public async ValueTask HandleAsync(SendMessage message, IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            probe.SendEntered.SetResult(message.Value);
            await probe.ReleaseSend.Task.WaitAsync(cancellationToken);
            probe.SendCompleted.SetResult();
        }
    }

    private sealed class RequestHandler(HandlerProbe probe) : IZLinkRequestHandler<RequestMessage, ReplyMessage>
    {
        public async ValueTask<ReplyMessage> HandleAsync(RequestMessage request, IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            Interlocked.Increment(ref probe.Requests);
            probe.InHandler?.Invoke();
            await Task.Yield();
            probe.InHandler?.Invoke();
            return new ReplyMessage(request.Value);
        }
    }

    private sealed class Peers(ServiceProvider server, ServiceProvider client) : IAsyncDisposable
    {
        public IZLinkRouteClient Client => client.GetRequiredService<IZLinkRouteClient>();
        public HandlerProbe Probe => server.GetRequiredService<HandlerProbe>();

        public static async Task<Peers> StartAsync()
        {
            int port;
            using (var listener = new TcpListener(IPAddress.Loopback, 0))
            {
                listener.Start();
                port = ((IPEndPoint)listener.LocalEndpoint).Port;
            }
            var serverServices = new ServiceCollection();
            serverServices.AddSingleton<HandlerProbe>();
            serverServices.AddZLinkFramework(options => options.AddClientServerChannel("work")
                .Server().Listen(port)
                .AddSendHandler<SendHandler, SendMessage>()
                .AddRequestHandler<RequestHandler, RequestMessage, ReplyMessage>());
            var clientServices = new ServiceCollection();
            clientServices.AddZLinkFramework(options => options.AddClientServerChannel("work")
                .Client().Connect($"tcp://127.0.0.1:{port}"));
            var peers = new Peers(serverServices.BuildServiceProvider(), clientServices.BuildServiceProvider());
            try
            {
                await peers.StartRuntimesAsync();
                return peers;
            }
            catch
            {
                await peers.DisposeAsync();
                throw;
            }
        }

        private async Task StartRuntimesAsync()
        {
            await server.GetRequiredService<ZLinkFrameworkRuntime>().StartAsync(CancellationToken.None);
            await client.GetRequiredService<ZLinkFrameworkRuntime>().StartAsync(CancellationToken.None);
        }

        public async ValueTask DisposeAsync()
        {
            Probe.ReleaseSend.TrySetResult();
            await client.GetRequiredService<ZLinkFrameworkRuntime>().StopAsync(CancellationToken.None);
            await server.GetRequiredService<ZLinkFrameworkRuntime>().StopAsync(CancellationToken.None);
            await client.DisposeAsync();
            await server.DisposeAsync();
        }
    }
}
