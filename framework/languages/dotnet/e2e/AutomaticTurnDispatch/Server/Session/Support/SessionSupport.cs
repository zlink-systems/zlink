namespace AutomaticTurnDispatch.Server.Session.Support;

using Zlink.Framework.E2E.Configuration;

internal sealed record NodeOptions(string Rid);

internal sealed class EvidenceStore
{
    private readonly List<string> _entries = [];
    private readonly string? _filePath;
    private readonly object _gate = new();

    public EvidenceStore(string rid, string? filePath)
    {
        Rid = rid;
        _filePath = filePath;
    }

    public string Rid { get; }

    public void Add(string entry)
    {
        lock (_gate)
        {
            _entries.Add(entry);
            if (!string.IsNullOrWhiteSpace(_filePath)) File.AppendAllText(_filePath, entry + Environment.NewLine);
        }
    }

    public string[] Snapshot()
    {
        lock (_gate)
        {
            return _entries.ToArray();
        }
    }
}

internal sealed record SessionOptions(
    string Rid,
    string HttpUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string ControlEndpoint,
    string PlayControlEndpoint,
    string SpotRouterEndpoint,
    string StreamEndpoint,
    string LogDir)
{
    public string EvidenceFile => Path.Combine(LogDir, $"{Rid}.evidence.log");

    public static SessionOptions Parse(string[] args)
        => E2eConfiguration.Load<SessionOptions>(args);
}
