using System;
using System.Collections.Concurrent;
using System.Threading;
using System.Threading.Tasks;

namespace Systems.Zlink.Tests;

internal sealed class SingleThreadSynchronizationContext : SynchronizationContext,
    IDisposable
{
    private readonly BlockingCollection<WorkItem> _queue = new();
    private readonly Thread _thread;
    private readonly ManualResetEventSlim _started = new(false);
    private volatile int _threadId;

    internal SingleThreadSynchronizationContext(string? name = null)
    {
        _thread = new Thread(Run)
        {
            IsBackground = true,
            Name = name ?? "Systems.Zlink.TestSynchronizationContext"
        };
        _thread.Start();
        _started.Wait();
    }

    internal int ThreadId => _threadId;

    public override void Post(SendOrPostCallback d, object? state)
    {
        _queue.Add(new WorkItem(d, state, null));
    }

    internal void Invoke(Action action)
    {
        Invoke<object?>(() =>
        {
            action();
            return null;
        });
    }

    internal T Invoke<T>(Func<T> action)
    {
        if (Environment.CurrentManagedThreadId == _threadId)
            return action();

        var completion = new TaskCompletionSource<T>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _queue.Add(new WorkItem(static state =>
        {
            var work = (InvocationWork<T>)state!;
            work.Run();
        }, new InvocationWork<T>(action, completion), null));
        return completion.Task.GetAwaiter().GetResult();
    }

    public void Dispose()
    {
        _queue.CompleteAdding();
        _thread.Join();
        _started.Dispose();
        _queue.Dispose();
    }

    private void Run()
    {
        SetSynchronizationContext(this);
        _threadId = Environment.CurrentManagedThreadId;
        _started.Set();

        foreach (WorkItem item in _queue.GetConsumingEnumerable())
        {
            try
            {
                item.Callback(item.State);
            }
            catch (Exception ex)
            {
                item.Completion?.SetException(ex);
            }
        }
    }

    private sealed class InvocationWork<T>
    {
        private readonly Func<T> _action;
        private readonly TaskCompletionSource<T> _completion;

        internal InvocationWork(Func<T> action, TaskCompletionSource<T> completion)
        {
            _action = action;
            _completion = completion;
        }

        internal void Run()
        {
            try
            {
                _completion.SetResult(_action());
            }
            catch (Exception ex)
            {
                _completion.SetException(ex);
            }
        }
    }

    private sealed class WorkItem
    {
        internal WorkItem(SendOrPostCallback callback, object? state,
            TaskCompletionSource<object?>? completion)
        {
            Callback = callback;
            State = state;
            Completion = completion;
        }

        internal SendOrPostCallback Callback { get; }
        internal object? State { get; }
        internal TaskCompletionSource<object?>? Completion { get; }
    }
}
