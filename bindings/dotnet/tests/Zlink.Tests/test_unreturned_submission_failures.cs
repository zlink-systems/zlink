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
            SubmitToMissingPeer(operation);
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            Assert.Empty(unobserved);
        }
        finally
        {
            TaskScheduler.UnobservedTaskException -= OnUnobserved;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void SubmitToMissingPeer(Operation operation)
    {
        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        router.Options.Mandatory = true;
        router.Bind(CoreTestSupport.NewEndpoint("inproc", "unreturned-submission"));
        using var completions = new CompletionPollerDriver(router);
        using var message = Message.From("unreturned");
        var missingPeer = RoutingId.From("departed-client");
        Assert.Throws<ZlinkSubmitException>(() =>
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
