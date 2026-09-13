namespace Zlink.Framework.UnitTests;

// Direct binding fixtures do not have a Framework receive loop. This driver
// gives those fixture sockets the same explicit public-poller completion owner
// that production runtimes provide.
internal sealed class TestCompletionPollerDriver : IDisposable
{
    private readonly IPoller _poller;
    private readonly Task _driver;
    private Exception? _failure;
    private bool _stopping;

    internal TestCompletionPollerDriver(IZlinkSocket socket)
    {
        _poller = Systems.Zlink.Zlink.CreatePoller();
        _poller.Add(socket, PollEventFlags.PollCompletion, 1);
        _driver = Task.Factory.StartNew(
            Drive,
            CancellationToken.None,
            TaskCreationOptions.LongRunning,
            TaskScheduler.Default);
    }

    public void Dispose()
    {
        Volatile.Write(ref _stopping, true);
        _driver.GetAwaiter().GetResult();
        _poller.Dispose();
        if (_failure is not null)
            throw new InvalidOperationException(
                "The test completion poller driver failed.",
                _failure);
    }

    private void Drive()
    {
        var events = new PollEvent[1];
        try
        {
            while (!Volatile.Read(ref _stopping))
                _poller.Wait(events, TimeSpan.FromMilliseconds(25));
        }
        catch (Exception exception)
        {
            _failure = exception;
        }
    }
}
