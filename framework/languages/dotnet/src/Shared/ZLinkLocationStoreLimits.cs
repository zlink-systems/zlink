namespace Zlink.Framework.Internal;

internal static class ZLinkLocationStoreLimits
{
    internal const int MaximumKeyBytes = 1024;

    internal const int MaximumValueBytes = 1024 * 1024;
    internal const int MaximumVersionBytes = 4096;
    internal const int MaximumCursorBytes = 4096;
    internal const int MaximumUniqueKeys = 2048;
    internal const int MaximumEncodedBatchBytes = 4 * 1024 * 1024;
    internal const int MaximumEncodedPageBytes = 4 * 1024 * 1024;
    internal const int MaximumPageItems = 1000;
}
