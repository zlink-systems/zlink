using DeliveryDispatch.Shared.Contracts;

namespace DeliveryDispatch.Server.Configuration;

public sealed class EvidenceStore
{
    private readonly object _gate = new();
    private readonly string _path;

    public EvidenceStore(SampleConfiguration configuration)
    {
        _path = configuration.EvidencePath;
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
    }

    public void Append(DeliveryStatusChangedReq status)
    {
        var line = string.Join(
            "|",
            status.DeliveryId,
            status.CustomerId,
            status.Status,
            status.CourierId ?? string.Empty,
            status.OccurredAtUnixMs.ToString());
        lock (_gate)
        {
            File.AppendAllLines(_path, [line]);
        }
    }

    public string[] ReadLines()
    {
        lock (_gate)
        {
            return File.Exists(_path)
                ? File.ReadAllLines(_path)
                : [];
        }
    }

    public bool HasSequence(string deliveryId, params DeliveryStatus[] expected)
    {
        var statuses = ReadLines()
            .Select(Parse)
            .Where(status => string.Equals(status.DeliveryId, deliveryId, StringComparison.Ordinal))
            .Select(status => status.Status)
            .ToArray();
        return statuses.SequenceEqual(expected);
    }

    private static DeliveryStatusChangedReq Parse(string line)
    {
        var parts = line.Split('|');
        return new DeliveryStatusChangedReq(
            parts[0],
            parts[1],
            Enum.Parse<DeliveryStatus>(parts[2]),
            string.IsNullOrEmpty(parts[3]) ? null : parts[3],
            long.Parse(parts[4]));
    }
}
