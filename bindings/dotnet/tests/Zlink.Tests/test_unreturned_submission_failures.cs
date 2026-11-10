using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_unreturned_submission_failures
{
    public enum Operation { Send, RequestSync, RequestAsync }

    [Theory]
    [InlineData(Operation.Send)]
    [InlineData(Operation.RequestSync)]
    [InlineData(Operation.RequestAsync)]
    public void immediate_failure_is_returned_to_caller_without_unobserved_tasks(Operation operation)
    {
        Exception returned = null!;
        var reported = CollectUnobserved(() => returned = SubmitToMissingPeer(operation));
        Assert.DoesNotContain(reported, error => ReferenceEquals(error, returned));
    }

    [Fact]
    public void pre_admission_failure_is_reported_once_through_reply()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());

        WeakReference admitted = null!;
        Exception replyError = null!;
        // The submission happens inside the collection window, so its own Admitted task is
        // finalized while the handler listens. The caller observes only Reply (#4171).
        var reported = CollectUnobserved(() =>
        {
            var (reply, weak) = SubmitBackpressuredThenClose();
            admitted = weak;
            var failure = Assert.Throws<AggregateException>(
                () => reply.Wait(TimeSpan.FromSeconds(5)));
            replyError = Assert.IsType<ZlinkSubmitException>(failure.InnerException);
        });
        Assert.False(admitted.IsAlive, "Admitted must be collectable for this check to mean anything");
        Assert.DoesNotContain(reported, error => ReferenceEquals(error, replyError));
    }

    // Everything except Reply is released when this returns, so Admitted becomes collectable.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (Task<IReadOnlyList<Message>> Reply, WeakReference Admitted)
        SubmitBackpressuredThenClose()
    {
        using var context = Zlink.CreateContext();
        var dealer = context.CreateDealerSocket();
        dealer.Connect(CoreTestSupport.NewEndpoint("inproc", "reply-only-pre-admission"));
        using var completions = Zlink.CreatePoller();
        completions.Add(dealer, PollEventFlags.PollCompletion, 0);
        using Message part = Message.From("reply-only");
        RequestSubmission pending = dealer.Request()
            .Message(part).Timeout(TimeSpan.FromSeconds(3)).Async();
        Assert.Equal(SubmitResult.Backpressured, pending.Result);
        Assert.False(pending.Admitted.IsCompleted);
        dealer.Dispose();
        return (pending.Reply, new WeakReference(pending.Admitted));
    }

    // Collects ZlinkSubmitException reports raised by finalized tasks. Other tests' unobserved
    // tasks are finalized before subscribing, and callers match only their own exception instance.
    private static List<Exception> CollectUnobserved(Action action)
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        var unobserved = new ConcurrentQueue<Exception>();
        void OnUnobserved(object? sender, UnobservedTaskExceptionEventArgs args)
        {
            foreach (var error in args.Exception.Flatten().InnerExceptions)
                if (error is ZlinkSubmitException)
                    unobserved.Enqueue(error);
        }

        TaskScheduler.UnobservedTaskException += OnUnobserved;
        try
        {
            action();
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            GC.WaitForPendingFinalizers();
            return unobserved.ToList();
        }
        finally
        {
            TaskScheduler.UnobservedTaskException -= OnUnobserved;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static Exception SubmitToMissingPeer(Operation operation)
    {
        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        router.Options.Mandatory = true;
        router.Bind(CoreTestSupport.NewEndpoint("inproc", "unreturned-submission"));
        using var completions = new CompletionPollerDriver(router);
        using var message = Message.From("unreturned");
        var missingPeer = RoutingId.From("departed-client");
        return Assert.Throws<ZlinkSubmitException>(() =>
        {
            switch (operation)
            {
                case Operation.Send:
                    _ = router.Send(missingPeer).Message(message).Async();
                    break;
                case Operation.RequestSync:
                    _ = router.Request(missingPeer).Message(message).Submit();
                    break;
                case Operation.RequestAsync:
                    _ = router.Request(missingPeer).Message(message).Async();
                    break;
                default:
                    throw new ArgumentOutOfRangeException(nameof(operation));
            }
        });
    }
}
