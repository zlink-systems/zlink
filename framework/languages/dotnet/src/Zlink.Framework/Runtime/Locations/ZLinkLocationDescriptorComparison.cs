using System.Text.Json;
using System.Text.Json.Nodes;

namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkLocationDescriptorComparison
{
    internal static bool SameExceptWriteTime<T>(T stored, T next)
    {
        var storedRecord = JsonSerializer.SerializeToNode(
            stored,
            ZLinkJsonSerializerOptions.Default
        )!;
        var nextRecord = JsonSerializer.SerializeToNode(next, ZLinkJsonSerializerOptions.Default)!;
        storedRecord.AsObject().Remove(ZLinkLocationRecordJsonFields.UpdatedAtEpochMs);
        nextRecord.AsObject().Remove(ZLinkLocationRecordJsonFields.UpdatedAtEpochMs);
        return JsonNode.DeepEquals(storedRecord, nextRecord);
    }
}
