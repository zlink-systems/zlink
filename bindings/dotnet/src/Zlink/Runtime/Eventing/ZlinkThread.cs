// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal sealed class ZlinkThread : IZlinkThread
{
    private readonly Thread _thread;
    private int _joined;

    public ZlinkThread(Action task)
    {
        if (task == null)
            throw new ArgumentNullException(nameof(task));

        _thread = new Thread(() =>
        {
            try
            {
                task();
            }
            catch (Exception ex)
            {
                CallbackExceptionHub.Report(ex);
                throw;
            }
        })
        {
            IsBackground = true,
            Name = "Systems.Zlink.Thread"
        };
        _thread.Start();
    }

    public void Join()
    {
        if (Interlocked.Exchange(ref _joined, 1) != 0)
            return;
        _thread.Join();
    }

    public void Close()
    {
        Dispose();
    }

    public void Dispose()
    {
        Join();
        GC.SuppressFinalize(this);
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }
}