namespace ZLink.Framework.Perf;

public sealed class ObjectsReadiness(bool ready, string reason)
{
    private sealed record State(bool Ready, string Reason, object[] Evidence);
    private volatile State state = new(ready, reason, []);
    public bool Ready => state.Ready;
    public string Reason => state.Reason;
    public object[] Evidence => state.Evidence;
    public void Set(bool ready, string reason, object[] evidence) => state = new(ready, reason, evidence);
}
