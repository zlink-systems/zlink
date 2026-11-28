namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Host relocation §14: the stop deadline passed while the unit's authority was
/// still undecided. The source changes nothing in the Store and does not call it
/// again; the Join caller gets <c>Unavailable</c> and retries.
/// </summary>
internal sealed class ZLinkAuthorityUnsettledAtStopException()
    : Exception("Actor relocation authority was unsettled at the stop deadline.");
