using System.Reflection;
using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

namespace Zlink.Framework.UnitTests;

public sealed class BackendStreamSocketConcurrencyTests
{
    private static readonly RoutingId NodeRid = RoutingId.From("stream-node");

    [Fact]
    public async Task ConcurrentBoundActorMessages_AreSubmittedSerially()
    {
        // RouteMesh 10.0.0 moved the bound-actor plane onto IStreamSessionService,
        // created lazily from the MeshNode. The framework wrapper still serialises
        // concurrent SendBoundActor submits under a single send gate; this exercises
        // that guarantee through the new session-service surface.
        var socket = DispatchProxy.Create<IStreamSocket, NoopStreamSocketProxy>();
        var node = DispatchProxy.Create<IMeshNode, StreamNodeProxy>();
        var session = DispatchProxy.Create<IStreamSessionService, BlockingSessionProxy>();
        var sessionProxy = (BlockingSessionProxy)(object)session;
        ((StreamNodeProxy)(object)node).Session = session;
        var sessionRid = RoutingId.From("session");
        sessionProxy.Bindings =
        [
            new StreamSessionBinding(
                sessionRid,
                new ActorRef("actor-1", 1, "actors", NodeRid),
                0,
                0),
            new StreamSessionBinding(
                sessionRid,
                new ActorRef("actor-2", 1, "actors", NodeRid),
                0,
                0)
        ];

        await using var backend = new ZLinkBackendStreamSocketWrapper(
            socket, node, completions: null, ownsNode: false);
        using var firstHeader = Message.From("first-header");
        using var firstBody = Message.From("first-body");
        using var secondHeader = Message.From("second-header");
        using var secondBody = Message.From("second-body");

        var first = Task.Run(() => backend.SendBoundActor(
            sessionRid,
            "actor-1",
            [firstHeader, firstBody],
            SendFlags.None));
        await sessionProxy.FirstSubmitEntered.Task.WaitAsync(TimeSpan.FromSeconds(2));

        var secondStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var second = Task.Run(() =>
        {
            secondStarted.TrySetResult();
            return backend.SendBoundActor(
                sessionRid,
                "actor-2",
                [secondHeader, secondBody],
                SendFlags.None);
        });
        await secondStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));

        await Task.Delay(100);
        Assert.Equal(1, sessionProxy.SubmitCount);
        Assert.Equal(1, sessionProxy.MaximumConcurrentSubmits);

        sessionProxy.AllowFirstSubmit.TrySetResult();
        Assert.True(await first.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.True(await second.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.Equal(2, sessionProxy.SubmitCount);
        Assert.Equal(1, sessionProxy.MaximumConcurrentSubmits);
    }

    private class NoopStreamSocketProxy : DispatchProxy
    {
        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            return targetMethod.Name switch
            {
                "DisposeAsync" => ValueTask.CompletedTask,
                "Dispose" => null,
                _ => throw new NotSupportedException(targetMethod.Name)
            };
        }
    }

    private class StreamNodeProxy : DispatchProxy
    {
        public IStreamSessionService Session { get; set; } = null!;

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            return targetMethod.Name switch
            {
                "CreateStreamSessionService" => Session,
                "DisposeAsync" => ValueTask.CompletedTask,
                "Dispose" => null,
                _ => throw new NotSupportedException(targetMethod.Name)
            };
        }
    }

    private class BlockingSessionProxy : DispatchProxy
    {
        private int _activeSubmits;
        private int _maximumConcurrentSubmits;
        private int _submitCount;

        public StreamSessionBinding[] Bindings { get; set; } = [];

        public TaskCompletionSource FirstSubmitEntered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource AllowFirstSubmit { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int MaximumConcurrentSubmits => Volatile.Read(ref _maximumConcurrentSubmits);

        public int SubmitCount => Volatile.Read(ref _submitCount);

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            switch (targetMethod.Name)
            {
                case "Start":
                    return null;
                case "Bindings":
                    return Bindings;
                case "SendToActor":
                    return Submit();
                case "DisposeAsync":
                    return ValueTask.CompletedTask;
                case "Dispose":
                    return null;
                default:
                    throw new NotSupportedException(targetMethod.Name);
            }
        }

        private SubmitResult Submit()
        {
            var active = Interlocked.Increment(ref _activeSubmits);
            UpdateMaximum(active);
            var submit = Interlocked.Increment(ref _submitCount);
            try
            {
                if (submit == 1)
                {
                    FirstSubmitEntered.TrySetResult();
                    AllowFirstSubmit.Task.GetAwaiter().GetResult();
                }

                return SubmitResult.Ok;
            }
            finally
            {
                Interlocked.Decrement(ref _activeSubmits);
            }
        }

        private void UpdateMaximum(int active)
        {
            while (true)
            {
                var current = Volatile.Read(ref _maximumConcurrentSubmits);
                if (current >= active) return;
                if (Interlocked.CompareExchange(ref _maximumConcurrentSubmits, active, current) == current)
                    return;
            }
        }
    }
}
