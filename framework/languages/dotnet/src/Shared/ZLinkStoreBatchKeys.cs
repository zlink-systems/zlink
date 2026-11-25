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
}
