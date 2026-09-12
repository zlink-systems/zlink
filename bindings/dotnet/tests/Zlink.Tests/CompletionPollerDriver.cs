namespace Systems.Zlink.Tests;

/// <summary>
///     Test caller that continuously drives a public PollCompletion owner.
/// </summary>
internal sealed class CompletionPollerDriver : IDisposable
{
    private readonly IPoller _poller;
    private readonly Task _driver;
    private Exception? _failure;
    private bool _stopping;

    internal CompletionPollerDriver(IZlinkSocket socket)
    {
        _poller = Zlink.CreatePoller();
        _poller.Add(socket, PollEventFlags.PollCompletion, 0);
        _driver = Task.Run(Drive);
    }

    public void Dispose()
    {
        Volatile.Write(ref _stopping, true);
        _driver.GetAwaiter().GetResult();
        _poller.Dispose();
        if (_failure is not null)
            throw new InvalidOperationException(
                "The public completion poller driver failed.", _failure);
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
