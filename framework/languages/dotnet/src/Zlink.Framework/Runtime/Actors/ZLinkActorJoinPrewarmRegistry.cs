namespace Zlink.Framework.Runtime.Actors;

/// <summary>
/// Target-side owner of the relocation temporary queue for cross-node
/// Actor Join admission (spec 15 §4.2). Once the target Spot's
/// <c>OnActorJoin</c> decides Accepted, the target registers this attempt
/// before Accepted returns to the source. Production ingress
/// (<c>ZLinkActorInboundPipeline</c>) consults this registry for an
/// ActorId that does not resolve to a local Actor yet — an arrival
/// between Accepted and Restore/PREPARE would otherwise be treated as
/// NotFound (the Actor genuinely does not exist locally yet) and dropped.
/// This registry parks such an arrival instead.
///
/// PREPARE (Restore) migrates every parked arrival into the real per-actor
/// handoff import atomically with removing the attempt — once migration
/// runs, ingress for this ActorId resolves through the normal actor
/// lookup (the real Actor now exists), so the attempt no longer needs to
/// exist. Only one attempt exists per object; a newer exact identity
/// (HandoffId) evicts an older one for the same object — newest attempt
/// wins (spec 15 §4.2 "같은 object의 relocation temporary queue는 하나만
/// 존재한다"). Expiry, rejection, and explicit abort all release the
/// attempt through the same path, failing any still-parked arrival
/// exactly once.
///
/// All mutating and lookup operations run under this instance's lock, so
/// no arrival can ever observe an in-between state where the object is
/// neither parkable nor about to be migrated.
/// </summary>
internal sealed class ZLinkActorJoinPrewarmRegistry
{
    internal readonly record struct ObjectKey(string ActorId, ulong ActorGeneration);

    /// <summary>
    /// One ingress arrival parked between Accepted and PREPARE, captured as
    /// a durable <see cref="ZLinkActorHandoffFrame"/> (via
    /// <see cref="ZLinkActorHandoffFrames.Capture"/>) so it outlives the
    /// live wire frame's own lifetime. <see cref="OnFailed"/> fires at most
    /// once if this arrival is evicted, rejected, or expires before
    /// PREPARE migrates it — never after a successful migration, whose
    /// outcome is then owned by the normal per-actor handoff import.
    /// </summary>
    internal readonly record struct ParkedMessage(
        ZLinkActorHandoffFrame Frame,
        Action OnFailed);

    internal enum IngressRoute
    {
        Parked,
        NotFound
    }

    private sealed class Attempt(string handoffId, ObjectKey objectKey)
    {
        internal string HandoffId { get; } = handoffId;
        internal ObjectKey ObjectKey { get; } = objectKey;
        internal List<ParkedMessage> Parked { get; } = [];
    }

    private readonly object _gate = new();
    private readonly Dictionary<string, Attempt> _byHandoff =
        new(StringComparer.Ordinal);
    private readonly Dictionary<ObjectKey, string> _byObject = [];

    /// <summary>
    /// Registers an attempt for a newly Accepted admission. A retried
    /// admission of the same HandoffId reuses the existing attempt. A
    /// different HandoffId already registered for the same object is
    /// evicted first (newest attempt wins) — its parked arrivals fail
    /// exactly once, and <paramref name="onEvicted"/> is invoked with the
    /// evicted HandoffId so the caller can release its own bookkeeping
    /// tied to that identity.
    /// </summary>
    internal void Register(
        string handoffId,
        string actorId,
        ulong actorGeneration,
        Action<string>? onEvicted = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(handoffId);
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        List<ParkedMessage>? evictedParked = null;
        string? evictedHandoffId = null;
        lock (_gate)
        {
            if (_byHandoff.ContainsKey(handoffId))
                return;
            var key = new ObjectKey(actorId, actorGeneration);
            if (_byObject.TryGetValue(key, out var displacedId)
                && !string.Equals(displacedId, handoffId, StringComparison.Ordinal)
                && _byHandoff.Remove(displacedId, out var displaced))
            {
                _byObject.Remove(key);
                evictedParked = displaced.Parked;
                evictedHandoffId = displacedId;
            }

            var attempt = new Attempt(handoffId, key);
            _byHandoff[handoffId] = attempt;
            _byObject[key] = handoffId;
        }

        if (evictedParked is not null)
            FailParked(evictedParked);
        if (evictedHandoffId is not null)
            onEvicted?.Invoke(evictedHandoffId);
    }

    /// <summary>
    /// Production ingress lookup (spec 15 §4.2). Parks the arrival if an
    /// attempt exists for the object, or reports not-found so the caller
    /// falls through to its normal NotFound handling — which also covers
    /// the case where PREPARE already migrated this object, since the
    /// real Actor then resolves the normal way.
    /// </summary>
    internal IngressRoute ParkOrDeliver(
        string actorId,
        ulong actorGeneration,
        Func<ZLinkActorHandoffFrame> captureFrame,
        Action onFailed)
    {
        ArgumentNullException.ThrowIfNull(captureFrame);
        ArgumentNullException.ThrowIfNull(onFailed);
        lock (_gate)
        {
            if (!_byObject.TryGetValue(
                    new ObjectKey(actorId, actorGeneration),
                    out var handoffId)
                || !_byHandoff.TryGetValue(handoffId, out var attempt))
                return IngressRoute.NotFound;
            attempt.Parked.Add(new ParkedMessage(
                captureFrame() with { ArrivalIndex = attempt.Parked.Count },
                onFailed));
            return IngressRoute.Parked;
        }
    }

    /// <summary>
    /// PREPARE (Restore) installs the real per-actor import and migrates
    /// every parked arrival into it, in arrival order, then removes the
    /// attempt — the atomic transition spec 15 §4.2 requires. From this
    /// point ingress for the object resolves the normal way (the real
    /// Actor now exists), so the attempt no longer needs to exist.
    /// </summary>
    /// <exception cref="InvalidOperationException">
    /// This identity was evicted by a newer exact identity before PREPARE
    /// reached this call — the late PREPARE for a dead identity must be
    /// discarded, not installed (spec 15 §4.2 "이전 identity의 늦은 chunk와
    /// Restore는 조립에 연결하지 않고 폐기한다").
    /// </exception>
    internal void CompleteMigration(
        string handoffId,
        Action<IReadOnlyList<ZLinkActorHandoffFrame>> deliver)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(handoffId);
        ArgumentNullException.ThrowIfNull(deliver);
        // Deliver under the same lock that removes the attempt. Delivering
        // after releasing the lock leaves a window where a concurrent
        // ParkOrDeliver sees the attempt gone (so it reports NotFound
        // instead of parking) while the real per-actor import has not yet
        // received the migrated frames — the arrival is then neither
        // parkable nor resolvable and is dropped. Running deliver here
        // keeps the transition atomic: no arrival can observe an
        // in-between state.
        lock (_gate)
        {
            if (!_byHandoff.Remove(handoffId, out var attempt))
                throw new InvalidOperationException(
                    "Actor Join prewarm attempt was evicted before PREPARE "
                    + "installed its stage: " + handoffId);
            if (_byObject.TryGetValue(attempt.ObjectKey, out var owner)
                && string.Equals(owner, handoffId, StringComparison.Ordinal))
                _byObject.Remove(attempt.ObjectKey);
            if (attempt.Parked.Count != 0)
                deliver([.. attempt.Parked.Select(static message => message.Frame)]);
        }
    }

    /// <summary>
    /// Releases the attempt for <paramref name="handoffId"/> — Rejected
    /// admission, expiry of the admission's validity window, or explicit
    /// abort. Any arrival still parked at release time fails exactly
    /// once. A no-op if the attempt was already migrated or evicted.
    /// </summary>
    internal void Release(string handoffId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(handoffId);
        List<ParkedMessage>? parked = null;
        lock (_gate)
        {
            if (_byHandoff.Remove(handoffId, out var attempt))
            {
                if (_byObject.TryGetValue(attempt.ObjectKey, out var owner)
                    && string.Equals(owner, handoffId, StringComparison.Ordinal))
                    _byObject.Remove(attempt.ObjectKey);
                parked = attempt.Parked;
            }
        }

        if (parked is not null)
            FailParked(parked);
    }

    internal bool IsEmpty
    {
        get
        {
            lock (_gate) return _byHandoff.Count == 0;
        }
    }

    private static void FailParked(List<ParkedMessage> parked)
    {
        foreach (var message in parked)
            message.OnFailed();
    }
}
