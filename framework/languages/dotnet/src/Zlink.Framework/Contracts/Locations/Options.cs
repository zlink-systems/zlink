namespace Zlink.Framework.Contracts.Locations;

/// <summary>
/// Location runtime policy options. A resolve observes the current location
/// state. A resolved spot handle retains its logical key and refreshes its
/// address according to the spot-address messaging contract.
/// </summary>
public sealed class ZLinkLocationOptions
{
    /// <summary>Owner lease renewal period. One write per runtime instance
    /// per interval; location rows are never written by heartbeat.</summary>
    public TimeSpan OwnerLeaseRenewInterval { get; set; } = TimeSpan.FromSeconds(5);

    /// <summary>Owner lease lifetime. Rows of an expired owner are treated
    /// as stale everywhere.</summary>
    public TimeSpan OwnerLeaseTtl { get; set; } = TimeSpan.FromSeconds(15);

    /// <summary>Store re-read period when the store has no watch support.
    /// Also bounds the staleness of the local owner lease snapshot.</summary>
    public TimeSpan PollingInterval { get; set; } = TimeSpan.FromSeconds(1);

    /// <summary>Grace boundary for location-state failures. Existing ready
    /// connections remain available; after this time, auto connect does not
    /// start new outbound connections until location state recovers.</summary>
    public TimeSpan StoreFailureGrace { get; set; } = TimeSpan.FromSeconds(30);

    /// <summary>
    /// Time reserved for local authority fencing before the owner lease
    /// expires.
    /// </summary>
    public TimeSpan OwnerLeaseFencingMargin { get; set; } =
        TimeSpan.FromSeconds(5);

    /// <summary>
    /// Maximum time allowed for one owner-lease renewal attempt.
    /// </summary>
    public TimeSpan OwnerLeaseRenewTimeout { get; set; } = TimeSpan.FromSeconds(3);

    /// <summary>
    /// Maximum age of a positive Ready object route retained by this runtime.
    /// Zero disables the route cache.
    /// </summary>
    public TimeSpan RouteCacheMaxAge { get; set; } = TimeSpan.FromSeconds(15);

    /// <summary>
    /// Maximum time that Message Follow relays traffic from the previous owner.
    /// Zero disables Message Follow.
    /// </summary>
    public TimeSpan MessageFollowDuration { get; set; } = TimeSpan.FromSeconds(30);

    /// <summary>
    /// Maximum time a relocated Session route seal may wait for its exact
    /// route update before the physical Session is closed.
    /// </summary>
    public TimeSpan SessionRelocationSealTimeout { get; set; } =
        TimeSpan.FromSeconds(3);

    /// <summary>
    /// Maximum size in bytes of one encoded relocation payload chunk sent as
    /// a relocationState command. Must be positive and must not exceed the
    /// transport frame limit.
    /// </summary>
    public long RelocationPayloadChunkLimit { get; set; } = 256 * 1024;

    /// <summary>
    /// Upper bound on the sum of relocation chunk bytes in flight on one peer
    /// connection. Zero disables the budget.
    /// </summary>
    public long RelocationInFlightPayloadBudget { get; set; } =
        16 * 1024 * 1024;

    /// <summary>
    /// Upper bound on the sum of relocation chunk bytes in flight across the
    /// whole node. Zero disables the budget.
    /// </summary>
    public long RelocationNodeInFlightPayloadBudget { get; set; }

    /// <summary>
    /// Time the target waits for the relocation cutover after restore, and
    /// the time the source retains the boundary batch copy for
    /// retransmission after the cutover submit reaches a terminal state.
    /// </summary>
    public TimeSpan RelocationCutoverWaitTimeout { get; set; } =
        TimeSpan.FromSeconds(1);
}
