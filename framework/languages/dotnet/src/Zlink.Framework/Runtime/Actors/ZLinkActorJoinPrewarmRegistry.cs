using Zlink.Framework.Runtime.Execution;

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
/// All mutating and lookup operations run on this instance's state lane, so
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
    private sealed class ParkedMessage(Action onFailed)
    {
        internal ZLinkActorHandoffFrame? Frame { get; private set; }
        internal Action OnFailed { get; } = onFailed;

        internal void SetFrame(ZLinkActorHandoffFrame frame) => Frame = frame;
    }

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

    private readonly record struct ParkedClaim(
        Attempt Attempt,
        ParkedMessage Message,
        int ArrivalIndex);

    private readonly ZLinkStateLane _lane = new();
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
        var evicted = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_byHandoff.ContainsKey(handoffId))
                return (Parked: (List<ParkedMessage>?)null, HandoffId: (string?)null);
            var key = new ObjectKey(actorId, actorGeneration);
            if (_byObject.TryGetValue(key, out var displacedId)
                && !string.Equals(displacedId, handoffId, StringComparison.Ordinal)
                && _byHandoff.Remove(displacedId, out var displaced))
            {
                _byObject.Remove(key);
                var parked = displaced.Parked;
                var evictedId = displacedId;
                var replacement = new Attempt(handoffId, key);
                _byHandoff[handoffId] = replacement;
                _byObject[key] = handoffId;
                return (Parked: parked, HandoffId: evictedId);
            }

            var attempt = new Attempt(handoffId, key);
            _byHandoff[handoffId] = attempt;
            _byObject[key] = handoffId;
            return (Parked: (List<ParkedMessage>?)null, HandoffId: (string?)null);
        }));

        if (evicted.Parked is not null)
            FailParked(evicted.Parked);
        if (evicted.HandoffId is not null)
            onEvicted?.Invoke(evicted.HandoffId);
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
        var claim = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!_byObject.TryGetValue(
                    new ObjectKey(actorId, actorGeneration),
                    out var handoffId)
                || !_byHandoff.TryGetValue(handoffId, out var attempt))
                return (ParkedClaim?)null;

            var message = new ParkedMessage(onFailed);
            var arrivalIndex = attempt.Parked.Count;
            attempt.Parked.Add(message);
            return new ParkedClaim(attempt, message, arrivalIndex);
        }));
        if (claim is null)
            return IngressRoute.NotFound;

        //  The placeholder claim is made on the lane before the caller-owned
        //  capture runs. Capture can re-enter production ingress, so it must
        //  not inherit the state owner as a callback nested in the lane turn.
        ZLinkActorHandoffFrame captured;
        try
        {
            captured = captureFrame() with { ArrivalIndex = claim.Value.ArrivalIndex };
        }
        catch
        {
            AwaitStateLane(_lane.RunAsync(() =>
            {
                if (_byHandoff.TryGetValue(claim.Value.Attempt.HandoffId, out var current)
                    && ReferenceEquals(current, claim.Value.Attempt))
                    current.Parked.Remove(claim.Value.Message);
            }));
            throw;
        }
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_byHandoff.TryGetValue(claim.Value.Attempt.HandoffId, out var current)
                && ReferenceEquals(current, claim.Value.Attempt)
                && current.Parked.Contains(claim.Value.Message))
                claim.Value.Message.SetFrame(captured);
        }));
        return IngressRoute.Parked;
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
        var frames = AwaitStateLane(_lane.RunAsync<IReadOnlyList<ZLinkActorHandoffFrame>>(() =>
        {
            if (!_byHandoff.Remove(handoffId, out var attempt))
                throw new InvalidOperationException(
                    "Actor Join prewarm attempt was evicted before PREPARE "
                    + "installed its stage: " + handoffId);
            if (_byObject.TryGetValue(attempt.ObjectKey, out var owner)
                && string.Equals(owner, handoffId, StringComparison.Ordinal))
                _byObject.Remove(attempt.ObjectKey);
            return attempt.Parked.Count == 0
                ? []
                : [.. attempt.Parked
                    .Where(static message => message.Frame is not null)
                    .Select(static message => message.Frame!)];
        }));
        if (frames.Count != 0)
            deliver(frames);
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
        var parked = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_byHandoff.Remove(handoffId, out var attempt))
            {
                if (_byObject.TryGetValue(attempt.ObjectKey, out var owner)
                    && string.Equals(owner, handoffId, StringComparison.Ordinal))
                    _byObject.Remove(attempt.ObjectKey);
                return attempt.Parked;
            }
            return null;
        }));

        if (parked is not null)
            FailParked(parked);
    }

    internal bool IsEmpty
    {
        get
        {
            return AwaitStateLane(_lane.RunAsync(() => _byHandoff.Count == 0));
        }
    }

    private static void FailParked(List<ParkedMessage> parked)
    {
        foreach (var message in parked)
            message.OnFailed();
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();
}
