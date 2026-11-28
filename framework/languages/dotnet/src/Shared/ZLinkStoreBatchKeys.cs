using System.Text;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.Internal;

internal static class ZLinkStoreBatchKeys
{
    internal static IReadOnlyList<ZLinkStoreKey> ForCondition(ZLinkStoreCondition condition) =>
        condition switch
        {
            ZLinkStoreCondition.Missing missing => [missing.Key],
            ZLinkStoreCondition.Version version => [version.Key],
            ZLinkStoreCondition.Value value => [value.Key],
            ZLinkStoreCondition.SumAtMost sum => sum.Keys
                ?? throw new ArgumentException("Store counter keys are required."),
            _ => throw new ArgumentException("Unknown Store condition."),
        };

    internal static ZLinkStoreKey ForMutation(ZLinkStoreMutation mutation) =>
        mutation switch
        {
            ZLinkStoreMutation.Put put => put.Key,
            ZLinkStoreMutation.Delete delete => delete.Key,
            ZLinkStoreMutation.Add add => add.Key,
            _ => throw new ArgumentException("Unknown Store mutation."),
        };

    internal static long EncodedBytes(
        ZLinkStoreWriteRequest request,
        IEnumerable<ZLinkStoreKey> conditionKeys,
        IEnumerable<ZLinkStoreKey> mutationKeys
    )
    {
        long bytes = 0;
        foreach (var key in conditionKeys.Concat(mutationKeys))
            bytes += Encoding.UTF8.GetByteCount(key.Value);

        foreach (var condition in request.Conditions)
            bytes += condition switch
            {
                ZLinkStoreCondition.Version version => Encoding.UTF8.GetByteCount(
                    version.Expected.Value
                ),
                ZLinkStoreCondition.Value value => value.Expected.Length,
                ZLinkStoreCondition.SumAtMost => 16,
                _ => 0,
            };
        foreach (var mutation in request.Mutations)
            bytes += mutation switch
            {
                ZLinkStoreMutation.Put put => put.Bytes.Length,
                ZLinkStoreMutation.Add => 8,
                _ => 0,
            };
        return bytes;
    }
}
