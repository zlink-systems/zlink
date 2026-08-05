// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;

namespace Systems.Zlink;

internal sealed class RequestCallState
{
    private CancellationTokenRegistration _cancellationRegistration;

    // Native completion, user cancellation, and timeout all share this gate;
    // the first terminal edge owns cleanup for the managed registrations.
    private int _completed;
    private System.Threading.Timer? _timeoutTimer;

    internal RequestCallState(TaskCompletionSource<Received> completion)
    {
        Completion = completion;
    }

    internal TaskCompletionSource<Received> Completion { get; }

    internal bool TrySetResult(Received received)
    {
        if (Interlocked.Exchange(ref _completed, 1) != 0)
            return false;
        DisposeRegistrations();
        return Completion.TrySetResult(received);
    }

    internal bool TrySetException(Exception error)
    {
        if (Interlocked.Exchange(ref _completed, 1) != 0)
            return false;
        DisposeRegistrations();
        return Completion.TrySetException(error);
    }

    internal bool TrySetCanceled(CancellationToken token)
    {
        if (Interlocked.Exchange(ref _completed, 1) != 0)
            return false;
        DisposeRegistrations();
        return Completion.TrySetCanceled(token);
    }

    internal void SetCancellationRegistration(
        CancellationTokenRegistration cancellationRegistration)
    {
        _cancellationRegistration = cancellationRegistration;
    }

    internal void SetTimeoutTimer(System.Threading.Timer? timeoutTimer)
    {
        _timeoutTimer = timeoutTimer;
    }

    internal void Dispose()
    {
        DisposeRegistrations();
    }

    internal static RequestCallState? FromUserData(object? userdata)
    {
        if (userdata is not GCHandle handle || !handle.IsAllocated)
            return null;

        try
        {
            return handle.Target as RequestCallState;
        }
        catch (InvalidOperationException)
        {
            return null;
        }
    }

    internal static void CancelFromUserData(object? userdata)
    {
        FromUserData(userdata)?.TrySetCanceled(CancellationToken.None);
    }

    internal static void TimeoutFromUserData(object? userdata)
    {
        FromUserData(userdata)?.TrySetException(
            new ZlinkRequestException(RequestResult.TimedOut));
    }

    private void DisposeRegistrations()
    {
        _timeoutTimer?.Dispose();
        _cancellationRegistration.Dispose();
    }
}

internal sealed class RequestCallState<T>
{
    private CancellationTokenRegistration _cancellationRegistration;
    private int _completed;
    private System.Threading.Timer? _timeoutTimer;

    internal RequestCallState(TaskCompletionSource<T> completion)
    {
        Completion = completion;
    }

    internal TaskCompletionSource<T> Completion { get; }

    internal bool TrySetResult(T result)
    {
        if (Interlocked.Exchange(ref _completed, 1) != 0)
            return false;
        DisposeRegistrations();
        return Completion.TrySetResult(result);
    }

    internal bool TrySetException(Exception error)
    {
        if (Interlocked.Exchange(ref _completed, 1) != 0)
            return false;
        DisposeRegistrations();
        return Completion.TrySetException(error);
    }

    internal bool TrySetCanceled(CancellationToken token = default)
    {
        if (Interlocked.Exchange(ref _completed, 1) != 0)
            return false;
        DisposeRegistrations();
        return token.CanBeCanceled
            ? Completion.TrySetCanceled(token)
            : Completion.TrySetCanceled();
    }

    internal void SetCancellationRegistration(
        CancellationTokenRegistration cancellationRegistration)
    {
        _cancellationRegistration = cancellationRegistration;
    }

    internal void SetTimeoutTimer(System.Threading.Timer? timeoutTimer)
    {
        _timeoutTimer = timeoutTimer;
    }

    internal void Dispose()
    {
        DisposeRegistrations();
    }

    internal static RequestCallState<T>? FromUserData(object? userdata)
    {
        if (userdata is not GCHandle handle || !handle.IsAllocated)
            return null;

        try
        {
            return handle.Target as RequestCallState<T>;
        }
        catch (InvalidOperationException)
        {
            return null;
        }
    }

    internal static void CancelFromUserData(object? userdata)
    {
        FromUserData(userdata)?.TrySetCanceled();
    }

    private void DisposeRegistrations()
    {
        _timeoutTimer?.Dispose();
        _cancellationRegistration.Dispose();
    }
}
