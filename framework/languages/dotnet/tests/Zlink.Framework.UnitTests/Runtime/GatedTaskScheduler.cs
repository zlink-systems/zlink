namespace Zlink.Framework.UnitTests;

internal sealed class GatedTaskScheduler : TaskScheduler
{
    private readonly Queue<Task> _tasks = new();
    private readonly TaskCompletionSource _queued =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private bool _released;

    internal Task Queued => _queued.Task;

    protected override IEnumerable<Task> GetScheduledTasks()
    {
        lock (_tasks)
            return _tasks.ToArray();
    }

    protected override bool TryExecuteTaskInline(Task task, bool taskWasPreviouslyQueued) => false;

    protected override void QueueTask(Task task)
    {
        lock (_tasks)
        {
            if (!_released)
            {
                _tasks.Enqueue(task);
                _queued.TrySetResult();
                return;
            }
        }
        Schedule(task);
    }

    internal void Release()
    {
        Task[] tasks;
        lock (_tasks)
        {
            _released = true;
            tasks = _tasks.ToArray();
            _tasks.Clear();
        }
        foreach (var task in tasks)
            Schedule(task);
    }

    private void Schedule(Task task) =>
        ThreadPool.QueueUserWorkItem(_ => TryExecuteTask(task));
}
