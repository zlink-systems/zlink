namespace Zlink.Framework.Internal;

internal static class ZLinkLocationStoreLimits
{
    internal const int MaximumKeyBytes = 1024;

    // Checklist C-2b: the authority record collapsed to one opaque row
    // (21-location-runtime.md#2.4) now embeds its payload as base64 inside
    // the same JSON value instead of a separate 1 MiB payload key. Spec §6
    // caps the underlying creation/authority payload at 1 MiB; base64
    // inflates that by ~4/3 plus JSON envelope overhead, so the per-key
    // value bound must be raised above the old 1 MiB to keep admitting a
    // maximum-size payload. This stays well under §8's 4 MiB whole-batch
    // bound.
    internal const int MaximumValueBytes = 2 * 1024 * 1024;
    internal const int MaximumVersionBytes = 4096;
    internal const int MaximumCursorBytes = 4096;
    internal const int MaximumUniqueKeys = 2048;
    internal const int MaximumEncodedBatchBytes = 4 * 1024 * 1024;
    internal const int MaximumEncodedPageBytes = 4 * 1024 * 1024;
    internal const int MaximumPageItems = 1000;
}
