using System.Collections.Concurrent;
using System.Diagnostics;
using SubmitAdmission.Shared;

namespace SubmitAdmission.Server.Infrastructure;

internal sealed class OperationEvidenceStore
{
    private sealed class MutableEvidence(string operationId, string family, string targetId)
    {
        public readonly object Gate = new();
        public string OperationId { get; } = operationId;
        public string Family { get; } = family;
        public string TargetId { get; } = targetId;
        public int PublicInvocationCount;
        public int InvalidInvocationCount;
        public DateTimeOffset? PendingAt;
        public int TransportAttemptCount;
        public int CommitCount;
        public int SendReadySignalCount;
        public int RetryAttemptCount;
        public int PendingWaiterCount;
        public int ReservationCount;
        public int CallbackCount;
        public int TerminalCount;
        public string? TerminalStatus;
        public DateTimeOffset? TerminalAt;
        public int HandlerEnteredCount;
        public DateTimeOffset? HandlerEnteredAt;
        public int HandlerCompletedCount;
        public DateTimeOffset? HandlerCompletedAt;
    }

    private readonly ConcurrentDictionary<string, MutableEvidence> _operations = new();
    private readonly ConcurrentDictionary<string, string> _traceOperations = new(StringComparer.Ordinal);
    private readonly string? _filePath;
    private readonly object _fileGate = new();
    private int _handlerCompletedCount;

    public int HandlerCompletedCount => Volatile.Read(ref _handlerCompletedCount);

    public OperationEvidenceStore(ServerOptions options)
    {
        _filePath = options.EvidenceFile;
        if (string.IsNullOrWhiteSpace(_filePath)) return;
        Directory.CreateDirectory(Path.GetDirectoryName(_filePath)!);
        File.WriteAllText(_filePath, string.Empty);
    }

    public void Invocation(string operationId, string family, string targetId, bool invalid = false)
    {
        var evidence = Get(operationId, family, targetId);
        lock (evidence.Gate)
        {
            evidence.PublicInvocationCount++;
            if (invalid) evidence.InvalidInvocationCount++;
        }
        Append($"invocation|operation={operationId}|family={family}|target={targetId}|invalid={invalid}");
    }

    public void Terminal(string operationId, string family, string targetId, string status)
    {
        var evidence = Get(operationId, family, targetId);
        lock (evidence.Gate)
        {
            evidence.TerminalCount++;
            evidence.TerminalStatus = status;
            evidence.TerminalAt = DateTimeOffset.UtcNow;
        }
        Append($"terminal|operation={operationId}|family={family}|target={targetId}|status={status}");
    }

    public void Pending(string operationId, string family, string targetId)
    {
        var evidence = Get(operationId, family, targetId);
        lock (evidence.Gate) evidence.PendingAt ??= DateTimeOffset.UtcNow;
        Append($"pending|operation={operationId}|family={family}|target={targetId}");
    }

    public void BindCurrentTrace(string operationId)
    {
        if (Activity.Current is { } activity)
            _traceOperations[activity.Id!] = operationId;
    }

    public void Admission(
        string traceId,
        string eventName,
        int pendingWaiterCount,
        int reservationCount,
        int callbackCount)
    {
        if (!_traceOperations.TryGetValue(traceId, out var operationId)
            || !_operations.TryGetValue(operationId, out var evidence))
            return;

        lock (evidence.Gate)
        {
            switch (eventName)
            {
                case "transport-attempt": evidence.TransportAttemptCount++; break;
                case "commit": evidence.CommitCount++; break;
                case "send-ready": evidence.SendReadySignalCount++; break;
                case "retry-attempt": evidence.RetryAttemptCount++; break;
            }
            evidence.PendingWaiterCount = pendingWaiterCount;
            evidence.ReservationCount = reservationCount;
            evidence.CallbackCount = callbackCount;
        }
        Append(
            $"admission|operation={operationId}|event={eventName}|pending={pendingWaiterCount}|reservations={reservationCount}|callbacks={callbackCount}");
        if (string.Equals(eventName, "cleanup", StringComparison.Ordinal))
            _traceOperations.TryRemove(traceId, out _);
    }

    public void HandlerEntered(string operationId, string family, string targetId)
    {
        var evidence = Get(operationId, family, targetId);
        lock (evidence.Gate)
        {
            evidence.HandlerEnteredCount++;
            evidence.HandlerEnteredAt ??= DateTimeOffset.UtcNow;
        }
        Append($"handler-entered|operation={operationId}|family={family}|target={targetId}");
    }

    public void HandlerCompleted(string operationId, string family, string targetId)
    {
        var evidence = Get(operationId, family, targetId);
        lock (evidence.Gate)
        {
            evidence.HandlerCompletedCount++;
            evidence.HandlerCompletedAt ??= DateTimeOffset.UtcNow;
        }
        Interlocked.Increment(ref _handlerCompletedCount);
        Append($"handler-completed|operation={operationId}|family={family}|target={targetId}");
    }

    public OperationEvidence? Snapshot(string operationId)
    {
        if (!_operations.TryGetValue(operationId, out var evidence)) return null;
        lock (evidence.Gate)
        {
            return new OperationEvidence(
                evidence.OperationId,
                evidence.Family,
                evidence.TargetId,
                evidence.PublicInvocationCount,
                evidence.InvalidInvocationCount,
                evidence.PendingAt,
                evidence.TransportAttemptCount,
                evidence.CommitCount,
                evidence.SendReadySignalCount,
                evidence.RetryAttemptCount,
                evidence.PendingWaiterCount,
                evidence.ReservationCount,
                evidence.CallbackCount,
                evidence.TerminalCount,
                evidence.TerminalStatus,
                evidence.TerminalAt,
                evidence.HandlerEnteredCount,
                evidence.HandlerEnteredAt,
                evidence.HandlerCompletedCount,
                evidence.HandlerCompletedAt);
        }
    }

    private MutableEvidence Get(string operationId, string family, string targetId) =>
        _operations.GetOrAdd(operationId, _ => new MutableEvidence(operationId, family, targetId));

    private void Append(string entry)
    {
        if (string.IsNullOrWhiteSpace(_filePath)) return;
        lock (_fileGate) File.AppendAllText(_filePath, entry + Environment.NewLine);
    }
}
