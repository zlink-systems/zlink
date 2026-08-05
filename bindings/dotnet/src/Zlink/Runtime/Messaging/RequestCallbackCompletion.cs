// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal sealed class RequestCallbackCompletion
{
    private readonly RequestCallback _callback;
    private int _completed;
    private RequestProgressPump.ProgressLease _progress;
    private int _progressState;

    internal RequestCallbackCompletion(RequestCallback callback)
    {
        _callback = callback;
    }

    internal void AttachProgress(RequestProgressPump.ProgressLease progress)
    {
        if (Volatile.Read(ref _completed) != 0)
        {
            progress.Dispose();
            return;
        }

        _progress = progress;
        if (Interlocked.CompareExchange(ref _progressState, 1, 0) == 0)
        {
            if (Volatile.Read(ref _completed) != 0
                && Interlocked.Exchange(ref _progressState, 2) == 1)
                _progress.Dispose();
            return;
        }

        progress.Dispose();
    }

    internal bool TryStartCompletion()
    {
        if (Interlocked.Exchange(ref _completed, 1) != 0)
            return false;
        DisposeProgress();
        return true;
    }

    internal void DisposeProgress()
    {
        if (Interlocked.Exchange(ref _progressState, 2) == 1)
            _progress.Dispose();
    }

    internal void Invoke(RequestResult result, IReadOnlyList<Message> parts)
    {
        try
        {
            _callback(result, parts);
        }
        catch (Exception ex)
        {
            CallbackExceptionHub.Report(ex);
        }
    }
}
