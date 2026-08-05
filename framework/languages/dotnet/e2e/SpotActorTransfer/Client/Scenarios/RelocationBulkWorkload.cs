using System.Collections.Concurrent;
using System.Diagnostics;
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal sealed class RelocationBulkWorkload(
    SpotActorTransferScenarioContext context,
    string scenario,
    string targetKind,
    IReadOnlyList<string> targetIds,
    int operationsPerSecond,
    ZLinkHttpClient? submittingNode = null,
    bool preservePerKindSubmissionOrder = false,
    bool resolveExpectedOwner = true)
{
    private readonly ConcurrentQueue<double> _requestLatencyMs = new();
    private readonly ConcurrentDictionary<string, ConcurrentQueue<long>>
        _acceptedRequests = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, ConcurrentQueue<long>>
        _acceptedOneWay = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, string>
        _requestOperationIds = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, string>
        _oneWayOperationIds = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, string>
        _actorRequestCorrelationByOperation =
            new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, RelocationObservedLocation>
        _latestRequestLocations = new(StringComparer.Ordinal);
    private long _requestOffered;
    private long _requestSubmitted;
    private long _requestSucceeded;
    private long _requestFailed;
    private long _oneWayOffered;
    private long _oneWaySubmitted;
    private long _oneWaySucceeded;
    private long _oneWayFailed;

    internal async Task WaitForInitialEvidenceAsync(
        TimeSpan timeout,
        bool requireAllTargets = false,
        CancellationToken cancellationToken = default)
        => await WaitForEvidenceAsync(
            requestWatermark:
                new HashSet<string>(StringComparer.Ordinal),
            oneWayWatermark:
                new HashSet<string>(StringComparer.Ordinal),
            requestSequenceExclusive: 0,
            oneWaySequenceExclusive: 0,
            observedAfterUnixTimeMilliseconds: 0,
            expectedOwners: null,
            requireAllTargets,
            timeout,
            cancellationToken);

    internal async Task WaitForAdditionalEvidenceAsync(
        TimeSpan timeout,
        bool requireAllTargets = false,
        CancellationToken cancellationToken = default)
    {
        var observedAfterUnixTimeMilliseconds =
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var requestSequenceExclusive = Interlocked.Read(
            ref _requestOffered);
        var oneWaySequenceExclusive = Interlocked.Read(
            ref _oneWayOffered);
        IReadOnlyDictionary<string, string>? expectedOwners = null;
        if (resolveExpectedOwner)
        {
            var locations = await context.GetRelocationLocationsAsync(
                context.NodeB,
                targetKind == "actor" ? targetIds : [],
                targetKind == "spot" ? targetIds : []);
            expectedOwners = locations.ToDictionary(
                static item => item.ObjectId,
                static item => item.NodeRid,
                StringComparer.Ordinal);
            ZlinkStreamAssert.Ensure(
                expectedOwners.Count == targetIds.Count,
                $"{scenario} final owner snapshot was incomplete.");
        }

        await WaitForEvidenceAsync(
            new HashSet<string>(StringComparer.Ordinal),
            new HashSet<string>(StringComparer.Ordinal),
            requestSequenceExclusive,
            oneWaySequenceExclusive,
            observedAfterUnixTimeMilliseconds,
            expectedOwners,
            requireAllTargets,
            timeout,
            cancellationToken);
    }

    private async Task WaitForEvidenceAsync(
        IReadOnlySet<string> requestWatermark,
        IReadOnlySet<string> oneWayWatermark,
        long requestSequenceExclusive,
        long oneWaySequenceExclusive,
        long observedAfterUnixTimeMilliseconds,
        IReadOnlyDictionary<string, string>? expectedOwners,
        bool requireAllTargets,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var sampleTargets = requireAllTargets
            ? targetIds.ToHashSet(StringComparer.Ordinal)
            : RepresentativeTargets();
        var coverageSeconds =
            (double)targetIds.Count / Math.Max(1, operationsPerSecond);
        var boundedTimeout = TimeSpan.FromSeconds(Math.Max(
            timeout.TotalSeconds,
            coverageSeconds + 5));
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        deadline.CancelAfter(boundedTimeout);
        while (true)
        {
            deadline.Token.ThrowIfCancellationRequested();
            var requestOperations = _requestOperationIds
                .Where(pair =>
                    ParseOperationSequence(pair.Key)
                    > requestSequenceExclusive
                    && !requestWatermark.Contains(pair.Value))
                .Select(static pair => pair.Value)
                .ToHashSet(StringComparer.Ordinal);
            var oneWayOperations = _oneWayOperationIds
                .Where(pair =>
                    ParseOperationSequence(pair.Key)
                    > oneWaySequenceExclusive
                    && !oneWayWatermark.Contains(pair.Value))
                .Select(static pair => pair.Value)
                .ToHashSet(StringComparer.Ordinal);
            if (requestOperations.Count > 0
                && oneWayOperations.Count > 0)
            {
                var evidence = (await Task.WhenAll(
                        context.GetEvidenceAsync(context.NodeA),
                        context.GetEvidenceAsync(context.NodeB),
                        context.GetEvidenceAsync(context.NodeC)))
                    .SelectMany(static items => items)
                    .ToArray();
                if (expectedOwners is not null)
                {
                    EnsureNoWrongOwnerEvidence(
                        evidence,
                        "workload_request",
                        requestOperations,
                        expectedOwners,
                        requestSequenceExclusive,
                        observedAfterUnixTimeMilliseconds);
                    EnsureNoWrongOwnerEvidence(
                        evidence,
                        "workload_one_way",
                        oneWayOperations,
                        expectedOwners,
                        oneWaySequenceExclusive,
                        observedAfterUnixTimeMilliseconds);
                }
                if (HasRepresentativeEvidence(
                        evidence,
                        sampleTargets,
                        "workload_request",
                        requestOperations,
                        expectedOwners)
                    && HasRepresentativeEvidence(
                        evidence,
                        sampleTargets,
                        "workload_one_way",
                        oneWayOperations,
                        expectedOwners))
                    return;
            }
            await Task.Delay(25, deadline.Token);
        }
    }

    private void EnsureNoWrongOwnerEvidence(
        IEnumerable<ActorEvidence> evidence,
        string kind,
        IReadOnlySet<string> operationIds,
        IReadOnlyDictionary<string, string> expectedOwners,
        long sequenceExclusive,
        long observedAfterUnixTimeMilliseconds)
    {
        var wrongOwner = evidence.FirstOrDefault(item =>
            item.Scenario == scenario
            && item.Kind == kind
            && item.ObservedUnixTimeMilliseconds
               >= observedAfterUnixTimeMilliseconds
            && operationIds.Contains(ParseHandlerOperationId(item.Value))
            && expectedOwners.TryGetValue(item.ActorId, out var expectedOwner)
            && ParseHandlerOwner(item.Value) != expectedOwner);
        ZlinkStreamAssert.Ensure(
            wrongOwner is null,
            $"{scenario} post-terminal {kind} reached stale owner "
            + $"'{ParseHandlerOwner(wrongOwner?.Value ?? string.Empty)}' "
            + $"for '{wrongOwner?.ActorId}'; "
            + $"sequence={ParseHandlerField(wrongOwner?.Value ?? string.Empty, "sequence")}; "
            + $"operation={ParseHandlerOperationId(wrongOwner?.Value ?? string.Empty)}; "
            + $"terminalSequence={sequenceExclusive}; "
            + $"observedAt={wrongOwner?.ObservedUnixTimeMilliseconds}; "
            + $"terminalObservedAt={observedAfterUnixTimeMilliseconds}.");
    }

    private IReadOnlySet<string> RepresentativeTargets()
    {
        const int maximumSamples = 16;
        var count = Math.Min(maximumSamples, targetIds.Count);
        var sample = new HashSet<string>(StringComparer.Ordinal);
        if (count == 1)
        {
            sample.Add(targetIds[0]);
            return sample;
        }

        for (var index = 0; index < count; index++)
        {
            var targetIndex = checked((int)Math.Round(
                (double)index * (targetIds.Count - 1) / (count - 1)));
            sample.Add(targetIds[targetIndex]);
        }
        return sample;
    }

    private bool HasRepresentativeEvidence(
        IEnumerable<ActorEvidence> evidence,
        IReadOnlySet<string> sampleTargets,
        string kind,
        IReadOnlySet<string> operationIds,
        IReadOnlyDictionary<string, string>? expectedOwners)
    {
        var observed = evidence
            .Where(item =>
                item.Scenario == scenario
                && item.Kind == kind
                && sampleTargets.Contains(item.ActorId)
                && operationIds.Contains(
                    ParseHandlerOperationId(item.Value))
                && (expectedOwners is null
                    || expectedOwners.TryGetValue(
                        item.ActorId,
                        out var expectedOwner)
                    && expectedOwner == ParseHandlerOwner(item.Value)))
            .Select(static item => item.ActorId)
            .ToHashSet(StringComparer.Ordinal);
        return observed.Count > 0
            && sampleTargets.All(observed.Contains);
    }

    private static string ParseHandlerOperationId(string value)
        => ParseHandlerField(value, "operation");

    internal static string ParseHandlerOwner(string value)
        => ParseHandlerField(value, "owner");

    private static string ParseHandlerField(string value, string name)
    {
        foreach (var field in value.Split(';'))
        {
            var pair = field.Split('=', 2);
            if (pair.Length == 2 && pair[0] == name)
                return pair[1];
        }
        return string.Empty;
    }

    public async Task<RelocationBulkWorkloadResult> RunAsync(
        TimeSpan duration,
        CancellationToken cancellationToken = default)
    {
        if (targetIds.Count == 0)
            throw new ArgumentException(
                "At least one workload target is required.",
                nameof(targetIds));

        var started = Stopwatch.StartNew();
        using var durationCancellation =
            CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken);
        durationCancellation.CancelAfter(duration);
        var operations = new ConcurrentBag<Task>();
        await Task.WhenAll(
            RunRequestPacerAsync(
                durationCancellation.Token,
                operations),
            RunOneWayPacerAsync(
                durationCancellation.Token,
                operations));
        await Task.WhenAll(operations.ToArray());

        started.Stop();
        return CreateResult(started.Elapsed);
    }

    internal async Task<RelocationBulkWorkloadResult> PrimeAllTargetsAsync(
        int maxConcurrency = 64,
        CancellationToken cancellationToken = default)
    {
        if (targetIds.Count == 0)
            throw new ArgumentException(
                "At least one workload target is required.",
                nameof(targetIds));
        var started = Stopwatch.StartNew();
        await Parallel.ForEachAsync(
            Enumerable.Range(0, targetIds.Count),
            new ParallelOptions
            {
                CancellationToken = cancellationToken,
                MaxDegreeOfParallelism = Math.Clamp(
                    maxConcurrency,
                    1,
                    256)
            },
            async (index, token) =>
            {
                var sequence = checked((long)index + 1);
                var targetId = targetIds[index];
                Interlocked.Increment(ref _requestOffered);
                Interlocked.Increment(ref _requestSubmitted);
                await ExecuteRequestAsync(sequence, targetId);
                Interlocked.Increment(ref _oneWayOffered);
                Interlocked.Increment(ref _oneWaySubmitted);
                await ExecuteOneWayAsync(sequence, targetId);
                token.ThrowIfCancellationRequested();
            });
        started.Stop();
        return CreateResult(started.Elapsed);
    }

    private RelocationBulkWorkloadResult CreateResult(TimeSpan elapsed)
    {
        return new RelocationBulkWorkloadResult(
            scenario,
            targetKind,
            elapsed,
            Interlocked.Read(ref _requestOffered),
            Interlocked.Read(ref _requestSubmitted),
            Interlocked.Read(ref _requestSucceeded),
            Interlocked.Read(ref _requestFailed),
            Interlocked.Read(ref _oneWayOffered),
            Interlocked.Read(ref _oneWaySubmitted),
            Interlocked.Read(ref _oneWaySucceeded),
            Interlocked.Read(ref _oneWayFailed),
            Percentile(_requestLatencyMs, 0.50),
            Percentile(_requestLatencyMs, 0.95),
            Percentile(_requestLatencyMs, 0.99),
            _requestLatencyMs.Count == 0
                ? 0
                : _requestLatencyMs.Max(),
            _acceptedRequests.ToDictionary(
                static pair => pair.Key,
                static pair =>
                    (IReadOnlyList<long>)pair.Value.ToArray(),
                StringComparer.Ordinal),
            _acceptedOneWay.ToDictionary(
                static pair => pair.Key,
                static pair => (IReadOnlyList<long>)pair.Value.ToArray(),
                StringComparer.Ordinal),
            new Dictionary<string, string>(
                _requestOperationIds,
                StringComparer.Ordinal),
            new Dictionary<string, string>(
                _oneWayOperationIds,
                StringComparer.Ordinal),
            new Dictionary<string, string>(
                _actorRequestCorrelationByOperation,
                StringComparer.Ordinal),
            new Dictionary<string, RelocationObservedLocation>(
                _latestRequestLocations,
                StringComparer.Ordinal));
    }

    internal RelocationBulkWorkloadResult Snapshot() =>
        CreateResult(TimeSpan.Zero);

    private async Task RunRequestPacerAsync(
        CancellationToken cancellationToken,
        ConcurrentBag<Task> operations)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(
            1d / Math.Max(1, operationsPerSecond)));
        try
        {
            while (await timer.WaitForNextTickAsync(cancellationToken))
            {
                // The offered watermark and the operation sequence are one
                // atomic value. A relocation terminal cannot observe an
                // operation between two separate counters and misclassify it
                // as post-terminal traffic.
                var current = Interlocked.Increment(ref _requestOffered);
                Interlocked.Increment(ref _requestSubmitted);
                var operation = ExecuteRequestAsync(current);
                if (preservePerKindSubmissionOrder)
                    await operation;
                else
                    operations.Add(operation);
            }
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
        }
    }

    private async Task RunOneWayPacerAsync(
        CancellationToken cancellationToken,
        ConcurrentBag<Task> operations)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(
            1d / Math.Max(1, operationsPerSecond)));
        try
        {
            while (await timer.WaitForNextTickAsync(cancellationToken))
            {
                var current = Interlocked.Increment(ref _oneWayOffered);
                Interlocked.Increment(ref _oneWaySubmitted);
                var operation = ExecuteOneWayAsync(current);
                if (preservePerKindSubmissionOrder)
                    await operation;
                else
                    operations.Add(operation);
            }
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
        }
    }

    private async Task ExecuteRequestAsync(
        long sequence,
        string? selectedTargetId = null)
    {
        var (targetId, submittingNode, request) = CreateCall(
            sequence,
            selectedTargetId);
        var requestWatch = Stopwatch.StartNew();
        try
        {
            var reply = targetKind == "actor"
                ? await context.RequestActorWorkloadAsync(
                    submittingNode,
                    request)
                : await context.RequestSpotWorkloadAsync(
                    submittingNode,
                    request);
            requestWatch.Stop();
            if (reply.Sequence != sequence
                || reply.TargetId != targetId)
            {
                Interlocked.Increment(ref _requestFailed);
                return;
            }
            if (reply.OperationId != request.OperationId
                || !reply.WithinDeadline
                || reply.ObjectGeneration <= 0)
            {
                Interlocked.Increment(ref _requestFailed);
                return;
            }
            if (targetKind == "actor"
                && string.IsNullOrWhiteSpace(reply.CorrelationId))
            {
                Interlocked.Increment(ref _requestFailed);
                return;
            }
            if (targetKind == "actor"
                && reply.CorrelationId is { Length: > 0 } correlationId)
            {
                const string connectionScope = "node-b";
                if (!_actorRequestCorrelationByOperation.TryAdd(
                        request.OperationId,
                        connectionScope + "\n" + correlationId))
                {
                    Interlocked.Increment(ref _requestFailed);
                    return;
                }
            }
            _latestRequestLocations.AddOrUpdate(
                targetId,
                _ => new RelocationObservedLocation(
                    sequence,
                    reply.NodeRid,
                    reply.ObjectGeneration),
                (_, current) => sequence > current.Sequence
                    ? new RelocationObservedLocation(
                        sequence,
                        reply.NodeRid,
                        reply.ObjectGeneration)
                    : current);

            _requestLatencyMs.Enqueue(
                requestWatch.Elapsed.TotalMilliseconds);
            _acceptedRequests
                .GetOrAdd(
                    targetId,
                    static _ => new ConcurrentQueue<long>())
                .Enqueue(sequence);
            _requestOperationIds.TryAdd(
                OperationKey(targetId, sequence),
                request.OperationId);
            Interlocked.Increment(ref _requestSucceeded);
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(
                $"relocation_workload_request_failed scenario={scenario}"
                + $" target={targetId}"
                + $" sequence={sequence}"
                + $" operation_id={request.OperationId}"
                + $" error_type={error.GetType().FullName}"
                + $" error={error.Message}");
            Interlocked.Increment(ref _requestFailed);
        }
    }

    private async Task ExecuteOneWayAsync(
        long sequence,
        string? selectedTargetId = null)
    {
        var (targetId, submittingNode, request) = CreateCall(
            sequence,
            selectedTargetId);
        try
        {
            if (targetKind == "actor")
                await context.SendActorWorkloadAsync(
                    submittingNode,
                    request);
            else
                await context.SendSpotWorkloadAsync(
                    submittingNode,
                    request);
            _acceptedOneWay
                .GetOrAdd(
                    targetId,
                    static _ => new ConcurrentQueue<long>())
                .Enqueue(sequence);
            _oneWayOperationIds.TryAdd(
                OperationKey(targetId, sequence),
                request.OperationId);
            Interlocked.Increment(ref _oneWaySucceeded);
        }
        catch (Exception)
        {
            Interlocked.Increment(ref _oneWayFailed);
        }
    }

    private (string TargetId, ZLinkHttpClient SubmittingNode,
        RelocationWorkloadCallReq Request) CreateCall(
            long sequence,
            string? selectedTargetId = null)
    {
        var targetId = selectedTargetId ?? targetIds[
            checked((int)((sequence - 1) % targetIds.Count))];
        // A single submitting connection gives the sequence an observable
        // transport arrival order. Alternating connections would only define
        // two independent orders and make a total-order assertion invalid.
        var selectedSubmittingNode = submittingNode ?? context.NodeB;
        var sent =
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        return (
            targetId,
            selectedSubmittingNode,
            new RelocationWorkloadCallReq(
                targetId,
                scenario,
                sequence,
                Guid.NewGuid().ToString("N"),
                sent,
                sent + 5_000));
    }

    internal static string OperationKey(
        string targetId,
        long sequence) =>
        targetId + "\n" + sequence;

    private static long ParseOperationSequence(string operationKey)
    {
        var separator = operationKey.LastIndexOf('\n');
        return separator >= 0
            && long.TryParse(
                operationKey.AsSpan(separator + 1),
                out var sequence)
            ? sequence
            : 0;
    }

    private static double Percentile(
        IEnumerable<double> source,
        double percentile)
    {
        var values = source.Order().ToArray();
        if (values.Length == 0)
            return 0;
        var index = (int)Math.Ceiling(
            percentile * values.Length) - 1;
        return values[Math.Clamp(index, 0, values.Length - 1)];
    }
}

internal sealed record RelocationBulkWorkloadResult(
    string Scenario,
    string TargetKind,
    TimeSpan Elapsed,
    long RequestOffered,
    long RequestSubmitted,
    long RequestSucceeded,
    long RequestFailed,
    long OneWayOffered,
    long OneWaySubmitted,
    long OneWaySucceeded,
    long OneWayFailed,
    double RequestP50Milliseconds,
    double RequestP95Milliseconds,
    double RequestP99Milliseconds,
    double RequestMaxMilliseconds,
    IReadOnlyDictionary<string, IReadOnlyList<long>>
        AcceptedRequests,
    IReadOnlyDictionary<string, IReadOnlyList<long>>
        AcceptedOneWay,
    IReadOnlyDictionary<string, string> RequestOperationIds,
    IReadOnlyDictionary<string, string> OneWayOperationIds,
    IReadOnlyDictionary<string, string>
        ActorRequestCorrelationByOperation,
    IReadOnlyDictionary<string, RelocationObservedLocation>
        LatestRequestLocations)
{
    public double RequestsPerSecond =>
        RequestSucceeded / Math.Max(
            Elapsed.TotalSeconds,
            double.Epsilon);
}

internal sealed record RelocationObservedLocation(
    long Sequence,
    string NodeRid,
    long ObjectGeneration);

internal sealed record RelocationTerminalSummary(
    int CompletedUnits,
    int VerifiedParticipants,
    int SafeAborted,
    int Blocked,
    double MaxServiceInterruptionMilliseconds,
    int ServiceUnitSloMissed);

internal readonly record struct SpotFlowWatermark(
    int NodeACount,
    int NodeBCount,
    int NodeCCount);

internal static class RelocationBulkWorkloadVerification
{
    internal static async Task VerifyAsync(
        SpotActorTransferScenarioContext context,
        RelocationBulkWorkloadResult result)
    {
        ZlinkStreamAssert.Ensure(
            result.RequestOffered == result.RequestSubmitted
            && result.RequestSubmitted
               == result.RequestSucceeded + result.RequestFailed
            && result.OneWayOffered == result.OneWaySubmitted
            && result.OneWaySubmitted
               == result.OneWaySucceeded + result.OneWayFailed
            && result.RequestFailed == 0
            && result.OneWayFailed == 0,
            $"{result.Scenario} {result.TargetKind} traffic failed: "
            + $"requestOffered={result.RequestOffered};"
            + $"requestSubmitted={result.RequestSubmitted};"
            + $"request={result.RequestFailed};"
            + $"oneWayOffered={result.OneWayOffered};"
            + $"oneWaySubmitted={result.OneWaySubmitted};"
            + $"oneWay={result.OneWayFailed}.");
        ZlinkStreamAssert.Ensure(
            result.RequestSucceeded > 0
            && result.OneWaySucceeded > 0,
            $"{result.Scenario} produced no accepted request or one-way "
            + "operation; zero-traffic evidence cannot pass.");

        if (result.TargetKind == "actor")
        {
            var requestOperations = result.RequestOperationIds.Values
                .ToArray();
            ZlinkStreamAssert.Ensure(
                result.ActorRequestCorrelationByOperation.Count
                    == result.RequestSucceeded
                && requestOperations.Length == result.RequestSucceeded
                && requestOperations.Distinct(StringComparer.Ordinal)
                       .Count() == requestOperations.Length
                && requestOperations.All(
                    result.ActorRequestCorrelationByOperation.ContainsKey)
                && result.ActorRequestCorrelationByOperation.Values
                       .Distinct(StringComparer.Ordinal).Count()
                   == result.ActorRequestCorrelationByOperation.Count,
                $"{result.Scenario} Actor request correlation was not "
                + "preserved one-to-one with the original operation, or "
                + "a duplicate reply was observed.");
        }

        IReadOnlyList<ActorEvidence> evidence = [];
        var deadline =
            DateTimeOffset.UtcNow + TimeSpan.FromSeconds(30);
        do
        {
            evidence = (await Task.WhenAll(
                    context.GetEvidenceAsync(context.NodeA),
                    context.GetEvidenceAsync(context.NodeB),
                    context.GetEvidenceAsync(context.NodeC)))
                .SelectMany(static items => items)
                .ToArray();
            if (HasAllOneWay(evidence, result))
                break;
            await Task.Delay(100);
        } while (DateTimeOffset.UtcNow < deadline);

        foreach (var (targetId, accepted) in
                 result.AcceptedRequests)
        {
            VerifyHandlerSet(
                evidence,
                result,
                targetId,
                accepted,
                "workload_request",
                result.RequestOperationIds);
        }
        foreach (var (targetId, accepted) in
                 result.AcceptedOneWay)
        {
            VerifyHandlerSet(
                evidence,
                result,
                targetId,
                accepted,
                "workload_one_way",
                result.OneWayOperationIds);
        }
    }

    private static void VerifyHandlerSet(
        IEnumerable<ActorEvidence> evidence,
        RelocationBulkWorkloadResult result,
        string targetId,
        IReadOnlyList<long> accepted,
        string kind,
        IReadOnlyDictionary<string, string> operationIds)
    {
        var handled = evidence
            .Where(item =>
                item.Scenario == result.Scenario
                && item.ActorId == targetId
                && item.Kind == kind)
            .Select(item => ParseHandlerEvidence(item.Value))
            .ToArray();
        ZlinkStreamAssert.Ensure(
            handled.Length == accepted.Count
            && handled.Select(static item => item.Sequence)
                   .Distinct().Count() == handled.Length
            && handled.Select(static item => item.Sequence)
                .Order()
                .SequenceEqual(accepted.Order())
            && (result.TargetKind != "actor"
                || handled.Select(static item => item.Sequence)
                    .SequenceEqual(accepted.Order()))
            && handled.All(item =>
                item.WithinDeadline
                && operationIds.TryGetValue(
                    RelocationBulkWorkload.OperationKey(
                        targetId,
                        item.Sequence),
                    out var expectedOperation)
                && expectedOperation == item.OperationId),
            $"{result.Scenario} {targetId} accepted {kind} "
            + "operation identity/deadline was lost or duplicated.");
    }

    private static (
        long Sequence,
        string OperationId,
        bool WithinDeadline) ParseHandlerEvidence(
        string value)
    {
        var fields = value.Split(';')
            .Select(static field => field.Split('=', 2))
            .Where(static field => field.Length == 2)
            .ToDictionary(
                static field => field[0],
                static field => field[1],
                StringComparer.Ordinal);
        return (
            long.Parse(fields["sequence"]),
            fields["operation"],
            bool.Parse(fields["withinDeadline"]));
    }

    private static bool HasAllOneWay(
        IEnumerable<ActorEvidence> evidence,
        RelocationBulkWorkloadResult result)
    {
        var snapshot = evidence.ToArray();
        return result.AcceptedOneWay.All(pair =>
            snapshot.Count(item =>
                item.Scenario == result.Scenario
                && item.ActorId == pair.Key
                && item.Kind == "workload_one_way")
            >= pair.Value.Count);
    }

    internal static void VerifyContinuity(
        RelocationBulkWorkloadResult baseline,
        RelocationBulkWorkloadResult relocation)
    {
        var minimumThroughput =
            baseline.RequestsPerSecond * 0.90;
        var maximumP99 = Math.Max(
            baseline.RequestP99Milliseconds * 2,
            250);
        ZlinkStreamAssert.Ensure(
            relocation.RequestsPerSecond >= minimumThroughput,
            $"{relocation.Scenario} throughput "
            + $"{relocation.RequestsPerSecond:F2}/s was below "
            + $"{minimumThroughput:F2}/s.");
        ZlinkStreamAssert.Ensure(
            relocation.RequestP99Milliseconds <= maximumP99,
            $"{relocation.Scenario} request p99 "
            + $"{relocation.RequestP99Milliseconds:F2} ms exceeded "
            + $"{maximumP99:F2} ms.");
    }

    internal static void Report(
        RelocationBulkWorkloadResult result)
    {
        var successRate = result.RequestSucceeded / Math.Max(
            1d,
            result.RequestSucceeded
            + result.RequestFailed) * 100;
        Console.WriteLine(
            $"{result.Scenario} kind={result.TargetKind}"
            + $" request_offered={result.RequestOffered}"
            + $" request_submitted={result.RequestSubmitted}"
            + $" requests={result.RequestSucceeded}"
            + $" request_errors={result.RequestFailed}"
            + $" one_way_offered={result.OneWayOffered}"
            + $" one_way_submitted={result.OneWaySubmitted}"
            + $" one_way_accepted={result.OneWaySucceeded}"
            + $" one_way_errors={result.OneWayFailed}"
            + $" success_rate={successRate:F3}%"
            + $" throughput={result.RequestsPerSecond:F2}/s"
            + $" p50_ms={result.RequestP50Milliseconds:F2}"
            + $" p95_ms={result.RequestP95Milliseconds:F2}"
            + $" p99_ms={result.RequestP99Milliseconds:F2}"
            + $" max_ms={result.RequestMaxMilliseconds:F2}"
            + $" correlation_count="
            + result.ActorRequestCorrelationByOperation.Count);
    }

    internal static async Task<RelocationTerminalSummary>
        VerifyRelocationTerminalsAsync(
        SpotActorTransferScenarioContext context,
        IReadOnlyList<RelocationLocationSnapshot> initial,
        IReadOnlyCollection<string> actorIds,
        IReadOnlyCollection<string> spotIds,
        IReadOnlyCollection<RelocationBulkWorkloadResult> traffic,
        bool requireSpotWideAggregatePublication,
        SpotFlowWatermark? spotFlowWatermark = null,
        IReadOnlyList<RelocationLocationSnapshot>? finalOverride = null)
    {
        var final = finalOverride
                    ?? await context.GetRelocationLocationsAsync(
                        context.NodeB,
                        actorIds,
                        spotIds);
        var initialByKey = initial.ToDictionary(LocationKey);
        var finalByKey = final.ToDictionary(LocationKey);
        ZlinkStreamAssert.Ensure(
            finalByKey.Count == actorIds.Count + spotIds.Count,
            "Relocation final location count is incomplete.");
        foreach (var (key, before) in initialByKey)
        {
            ZlinkStreamAssert.Ensure(
                finalByKey.TryGetValue(key, out var after)
                && after.ObjectGeneration
                   == before.ObjectGeneration
                && after.NodeRid != before.NodeRid,
                $"Relocation final owner/generation mismatch for "
                + $"{before.ObjectKind} '{before.ObjectId}'.");
        }

        var handlerEvidence = (await Task.WhenAll(
                context.GetEvidenceAsync(context.NodeA),
                context.GetEvidenceAsync(context.NodeB),
                context.GetEvidenceAsync(context.NodeC)))
            .SelectMany(static items => items)
            .ToArray();
        foreach (var result in traffic)
        {
            foreach (var targetId in result.AcceptedRequests.Keys
                         .Concat(result.AcceptedOneWay.Keys)
                         .Distinct(StringComparer.Ordinal))
            {
                var kind = result.TargetKind;
                if (!finalByKey.TryGetValue(
                        kind + "\n" + targetId,
                        out var location))
                    continue;
                var targetAdmissions = handlerEvidence.Where(item =>
                    item.Scenario == result.Scenario
                    && item.ActorId == targetId
                    && item.Kind is "workload_request"
                        or "workload_one_way"
                    && RelocationBulkWorkload.ParseHandlerOwner(item.Value)
                       == location.NodeRid);
                ZlinkStreamAssert.Ensure(
                    targetAdmissions.Any(),
                    $"No post-terminal handler evidence reached final owner "
                    + $"'{location.NodeRid}' for {kind} '{targetId}'.");
            }
        }

        var messageFlows = await WaitForMessageFlowsAsync(
            context,
            traffic,
            spotFlowWatermark
                ?? default,
            TimeSpan.FromSeconds(5));
        VerifySpotRequestCorrelation(messageFlows, traffic);

        // A final-owner snapshot cannot prove that a SpotWide aggregate was
        // never partially visible. Until the process E2E records both sides
        // of the publication boundary, this contract remains blocked.
        var aggregatePublicationBlocked =
            requireSpotWideAggregatePublication ? 1 : 0;
        if (aggregatePublicationBlocked != 0)
            Console.WriteLine(
                "required_gap=spotwide_pre_post_visibility"
                + " status=not_proven"
                + " reason=no_public_atomic_publication_observer");

        var interruptions = MeasureServiceUnitInterruptions(
            initialByKey,
            finalByKey,
            handlerEvidence,
            traffic,
            requireSpotWideAggregatePublication
                ? spotIds
                : actorIds.Concat(spotIds).ToArray());

        return new RelocationTerminalSummary(
            CompletedUnits: requireSpotWideAggregatePublication
                ? spotIds.Count
                : actorIds.Count + spotIds.Count,
            VerifiedParticipants: final.Count,
            SafeAborted: 0,
            Blocked: aggregatePublicationBlocked,
            MaxServiceInterruptionMilliseconds:
                interruptions.MaxMilliseconds,
            ServiceUnitSloMissed: interruptions.SloMissed);
    }

    private static (double MaxMilliseconds, int SloMissed)
        MeasureServiceUnitInterruptions(
            IReadOnlyDictionary<string, RelocationLocationSnapshot> initial,
            IReadOnlyDictionary<string, RelocationLocationSnapshot> final,
            IReadOnlyCollection<ActorEvidence> evidence,
            IReadOnlyCollection<RelocationBulkWorkloadResult> traffic,
            IReadOnlyCollection<string> serviceUnitIds)
    {
        var scenariosByTarget = traffic
            .SelectMany(result => result.AcceptedRequests.Keys
                .Concat(result.AcceptedOneWay.Keys)
                .Distinct(StringComparer.Ordinal)
                .Select(targetId => (targetId, result.Scenario)))
            .GroupBy(static item => item.targetId, StringComparer.Ordinal)
            .ToDictionary(
                static group => group.Key,
                static group => group
                    .Select(static item => item.Scenario)
                    .ToHashSet(StringComparer.Ordinal),
                StringComparer.Ordinal);
        var maximum = 0d;
        var missed = 0;
        foreach (var targetId in serviceUnitIds)
        {
            var actorKey = "actor\n" + targetId;
            var spotKey = "spot\n" + targetId;
            var key = initial.ContainsKey(actorKey)
                ? actorKey
                : spotKey;
            if (!initial.TryGetValue(key, out var before)
                || !final.TryGetValue(key, out var after)
                || !scenariosByTarget.TryGetValue(
                    targetId,
                    out var scenarios))
            {
                ZlinkStreamAssert.Ensure(
                    false,
                    $"Service interruption evidence could not resolve "
                    + $"'{targetId}'.");
                continue;
            }
            var observations = evidence
                .Where(item =>
                    item.ActorId == targetId
                    && scenarios.Contains(item.Scenario)
                    && item.Kind is "workload_request"
                        or "workload_one_way")
                .ToArray();
            var sourceLast = observations
                .Where(item =>
                    RelocationBulkWorkload.ParseHandlerOwner(item.Value)
                    == before.NodeRid)
                .Select(static item =>
                    item.ObservedUnixTimeMilliseconds)
                .DefaultIfEmpty(long.MinValue)
                .Max();
            var targetFirst = observations
                .Where(item =>
                    RelocationBulkWorkload.ParseHandlerOwner(item.Value)
                    == after.NodeRid)
                .Select(static item =>
                    item.ObservedUnixTimeMilliseconds)
                .DefaultIfEmpty(long.MaxValue)
                .Min();
            ZlinkStreamAssert.Ensure(
                sourceLast != long.MinValue
                && targetFirst != long.MaxValue,
                $"Service interruption evidence is incomplete for "
                + $"'{targetId}': sourceLast={sourceLast};"
                + $"targetFirst={targetFirst}.");
            var milliseconds = Math.Max(
                0,
                checked((double)(targetFirst - sourceLast)));
            maximum = Math.Max(maximum, milliseconds);
            if (milliseconds > 1_000)
                missed++;
        }

        return (maximum, missed);
    }

    private static void VerifySpotRequestCorrelation(
        IReadOnlyCollection<RelocationMessageFlowEvidence> messageFlows,
        IReadOnlyCollection<RelocationBulkWorkloadResult> traffic)
    {
        foreach (var expected in ExpectedSpotRequestCounts(traffic))
        {
            var completedRequests = CountCompletedSpotRequestCorrelations(
                messageFlows,
                [expected.Key]);
            ZlinkStreamAssert.Ensure(
                completedRequests == expected.Value,
                $"Spot request public message-flow correlation evidence "
                + $"is incomplete for '{expected.Key}': "
                + $"succeeded={expected.Value}, "
                + $"completed_requests={completedRequests}; "
                + DescribeSpotRequestCorrelation(
                    messageFlows,
                    [expected.Key]));
        }
    }

    private static async Task<RelocationMessageFlowEvidence[]>
        WaitForMessageFlowsAsync(
            SpotActorTransferScenarioContext context,
            IReadOnlyCollection<RelocationBulkWorkloadResult> traffic,
            SpotFlowWatermark flowWatermark,
            TimeSpan timeout)
    {
        using var deadline = new CancellationTokenSource(timeout);
        RelocationMessageFlowEvidence[] snapshot = [];
        do
        {
            var nodeSnapshots = await Task.WhenAll(
                context.GetRelocationMessageFlowsAsync(context.NodeA),
                context.GetRelocationMessageFlowsAsync(context.NodeB),
                context.GetRelocationMessageFlowsAsync(context.NodeC));
            snapshot = nodeSnapshots[0].Skip(flowWatermark.NodeACount)
                .Concat(nodeSnapshots[1].Skip(flowWatermark.NodeBCount))
                .Concat(nodeSnapshots[2].Skip(flowWatermark.NodeCCount))
                .ToArray();
            if (ExpectedSpotRequestCounts(traffic)
                .All(expected => CountCompletedSpotRequestCorrelations(
                        snapshot,
                        [expected.Key])
                    >= expected.Value))
                return snapshot;
            try
            {
                await Task.Delay(25, deadline.Token);
            }
            catch (OperationCanceledException)
            {
                return snapshot;
            }
        } while (!deadline.IsCancellationRequested);

        return snapshot;
    }

    private static IReadOnlyDictionary<string, int>
        ExpectedSpotRequestCounts(
            IEnumerable<RelocationBulkWorkloadResult> traffic) =>
        traffic
            .Where(static result =>
                result.TargetKind == "spot"
                && result.RequestSucceeded > 0)
            .SelectMany(static result => result.AcceptedRequests)
            .GroupBy(static pair => pair.Key, StringComparer.Ordinal)
            .ToDictionary(
                static group => group.Key,
                static group => group.Sum(pair => pair.Value.Count),
                StringComparer.Ordinal);

    internal static async Task<SpotFlowWatermark>
        CaptureSpotFlowWatermarkAsync(
            SpotActorTransferScenarioContext context)
    {
        var snapshots = await Task.WhenAll(
            context.GetRelocationMessageFlowsAsync(context.NodeA),
            context.GetRelocationMessageFlowsAsync(context.NodeB),
            context.GetRelocationMessageFlowsAsync(context.NodeC));
        return new SpotFlowWatermark(
            snapshots[0].Count,
            snapshots[1].Count,
            snapshots[2].Count);
    }

    private static int CountCompletedSpotRequestCorrelations(
        IReadOnlyCollection<RelocationMessageFlowEvidence> messageFlows,
        IEnumerable<string> targetIds) =>
        SelectSpotRequestFlows(messageFlows, targetIds)
            .GroupBy(
                static item => (item.FlowId!, item.CorrelationId!))
            .Count(group =>
            {
                // Message Follow may produce more than one Framework receive
                // boundary while preserving the original request identity.
                // The application terminal remains exactly one reply.
                var received = group.Count(static item =>
                    item.Phase == "received");
                var replied = group.Count(static item =>
                    item.Phase == "replied");
                return received >= 1 && replied == 1;
            });

    private static IEnumerable<RelocationMessageFlowEvidence>
        SelectSpotRequestFlows(
            IEnumerable<RelocationMessageFlowEvidence> messageFlows,
            IEnumerable<string> targetIds)
    {
        var targets = targetIds.ToHashSet(StringComparer.Ordinal);
        return messageFlows
            .Where(item =>
                item.Surface == "spot"
                && item.MessageKind == "request"
                && item.PacketName == nameof(RelocationWorkloadRequest)
                && item.SpotId is not null
                && targets.Contains(item.SpotId)
                && item.CorrelationId is { Length: > 0 }
                && item.FlowId is { Length: > 0 });
    }

    private static string DescribeSpotRequestCorrelation(
        IReadOnlyCollection<RelocationMessageFlowEvidence> messageFlows,
        IEnumerable<string> targetIds)
    {
        var filtered = SelectSpotRequestFlows(messageFlows, targetIds)
            .ToArray();
        var groups = filtered
            .GroupBy(
                static item => (item.FlowId!, item.CorrelationId!))
            .Select(group => new
            {
                Received = group.Count(static item =>
                    item.Phase == "received"),
                Replied = group.Count(static item =>
                    item.Phase == "replied")
            })
            .ToArray();
        var patterns = groups
            .GroupBy(static item => (item.Received, item.Replied))
            .OrderByDescending(static group => group.Count())
            .Take(6)
            .Select(group =>
                $"{group.Key.Received}:{group.Key.Replied}"
                + $"x{group.Count()}");
        var nodes = filtered
            .GroupBy(static item => item.NodeRid, StringComparer.Ordinal)
            .OrderBy(static group => group.Key, StringComparer.Ordinal)
            .Select(group => $"{group.Key}:{group.Count()}");
        return $"events={filtered.Length}, "
               + $"received={filtered.Count(static item => item.Phase == "received")}, "
               + $"replied={filtered.Count(static item => item.Phase == "replied")}, "
               + $"flows={filtered.Select(static item => item.FlowId).Distinct(StringComparer.Ordinal).Count()}, "
               + $"correlations={filtered.Select(static item => item.CorrelationId).Distinct(StringComparer.Ordinal).Count()}, "
               + $"pairs={groups.Length}, "
               + $"patterns=[{string.Join(",", patterns)}], "
               + $"nodes=[{string.Join(",", nodes)}].";
    }

    private static string LocationKey(
        RelocationLocationSnapshot item) =>
        item.ObjectKind + "\n" + item.ObjectId;
}

internal static class RelocationWorkloadEnvironment
{
    internal static bool Enabled(string name, bool fallback)
    {
        _ = name;
        return fallback;
    }

    internal static int Count(string name, int canonical) =>
        PositiveInt(name, canonical);

    internal static TimeSpan Duration(
        string name,
        int canonicalSeconds) =>
        TimeSpan.FromSeconds(
            PositiveInt(name, canonicalSeconds));

    internal static int Rate(string name, int canonical) =>
        PositiveInt(name, canonical);

    private static int PositiveInt(
        string name,
        int fallback)
    {
        _ = name;
        return fallback;
    }
}
