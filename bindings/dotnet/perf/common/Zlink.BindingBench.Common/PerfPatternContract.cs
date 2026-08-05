using System;
using System.Collections.Generic;

public interface IPerfPattern
{
    string Name { get; }
    int RunSingle(PerfOptions options);
    int RunMultiServer(PerfOptions options);
    int RunMultiClient(PerfOptions options);
}

public sealed class PerfPatternRegistry
{
    private readonly Dictionary<string, IPerfPattern> _patterns;

    public PerfPatternRegistry(IEnumerable<IPerfPattern> patterns)
    {
        _patterns = new Dictionary<string, IPerfPattern>(
            StringComparer.OrdinalIgnoreCase);
        foreach (IPerfPattern pattern in patterns)
            _patterns[pattern.Name] = pattern;
    }

    public bool TryGet(string name, out IPerfPattern pattern)
    {
        return _patterns.TryGetValue(name, out pattern!);
    }
}
