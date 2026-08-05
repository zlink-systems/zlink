using System.Collections.Concurrent;

namespace ChannelEgressRouting.Server;

internal sealed class EvidenceStore
{
    private readonly ConcurrentQueue<string> _entries = new();
    private readonly object _fileGate = new();
    private readonly string _file;

    public EvidenceStore(RoleOptions options)
    {
        _file = options.EvidenceFile;
        Directory.CreateDirectory(Path.GetDirectoryName(_file)!);
        File.WriteAllText(_file, string.Empty);
    }

    public void Add(string entry)
    {
        _entries.Enqueue(entry);
        lock (_fileGate)
            File.AppendAllText(_file, entry + Environment.NewLine);
    }

    public string[] Snapshot() => _entries.ToArray();
}
