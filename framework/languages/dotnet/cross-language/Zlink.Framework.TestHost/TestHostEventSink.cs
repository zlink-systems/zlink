using System.Collections.Concurrent;

// One writer thread appends event and flow lines in enqueue order, so Framework
// completion threads never block on file I/O (#5905). Linked by the unit tests
// and the gRPC bench client together with TestHostMessageFlowListener.cs.
internal sealed class TestHostEventSink : IDisposable
{
    private readonly string? _path;
    private readonly BlockingCollection<Action> _writes = new();
    private readonly Task _writer;

    public TestHostEventSink(string? path)
    {
        _path = path;
        _writer = Task.Factory.StartNew(
            () =>
            {
                foreach (var write in _writes.GetConsumingEnumerable())
                    write();
            },
            CancellationToken.None,
            TaskCreationOptions.LongRunning,
            TaskScheduler.Default
        );
    }

    public void Append(string value)
    {
        if (!string.IsNullOrWhiteSpace(_path))
            Append(_path, value);
    }

    public void Append(string path, string value) =>
        _writes.Add(() =>
        {
            var directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrWhiteSpace(directory))
                Directory.CreateDirectory(directory);
            File.AppendAllText(path, value + Environment.NewLine);
        });

    public string[] ReadLines(string path)
    {
        var read = new TaskCompletionSource<string[]>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        _writes.Add(() => read.SetResult(File.ReadAllLines(path)));
        var completed = Task.WhenAny(read.Task, _writer).GetAwaiter().GetResult();
        completed.GetAwaiter().GetResult();
        return read.Task.GetAwaiter().GetResult();
    }

    public void Dispose()
    {
        _writes.CompleteAdding();
        try
        {
            _writer.GetAwaiter().GetResult();
        }
        finally
        {
            _writes.Dispose();
        }
    }
}
