using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkInstanceSpotMonitoring
{
    private enum ActivationOutcome
    {
        Ready,
        Rejected,
        TimedOut,
        Shutdown,
        Fenced,
        Conflict,
        StoreFailure,
    }

    private static string OutcomeKey(ActivationOutcome outcome) =>
        outcome switch
        {
            ActivationOutcome.Ready => "ready",
            ActivationOutcome.Rejected => "rejected",
            ActivationOutcome.TimedOut => "timed_out",
            ActivationOutcome.Shutdown => "shutdown",
            ActivationOutcome.Fenced => "fenced",
            ActivationOutcome.Conflict => "conflict",
            ActivationOutcome.StoreFailure => "store_failure",
            _ => throw new ArgumentOutOfRangeException(nameof(outcome)),
        };

    private readonly ConcurrentDictionary<string, Operation> _operations = new(
        StringComparer.Ordinal
    );
    private readonly ConcurrentDictionary<string, Aggregate> _aggregates = new(
        StringComparer.Ordinal
    );

    internal async Task<InstanceSpotActivationTerminal> ObserveAsync(
        string operationKey,
        string meshName,
        string stableType,
        ulong pendingBytes,
        Func<Task<InstanceSpotActivationTerminal>> operation,
        InstanceSpotActivationOperation activation,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        ZLinkMessageFlowTracer? flow
    )
    {
        var selected = _operations.GetOrAdd(
            operationKey,
            _ => new Operation(
                stableType,
                ZLinkRuntimeMetrics.StartInstanceSpotActivation(meshName, stableType)
            )
        );
        if (!StringComparer.Ordinal.Equals(selected.StableType, stableType))
            throw new InvalidOperationException(
                $"Instance operation '{operationKey}' changed stable type."
            );

        var aggregate = _aggregates.GetOrAdd(stableType, static _ => new Aggregate());
        if (selected.TryAccept())
            aggregate.Accept(pendingBytes);
        Exception? failure = null;
        try
        {
            var terminal = await operation().ConfigureAwait(false);
            if (terminal.Result != RequestResult.Ok)
                failure = ZLinkRequestFailureMapper.CreateCompletionException(
                    terminal.Result,
                    (int)terminal.FailureCode,
                    "Instance Spot activation"
                );
            return terminal;
        }
        catch (Exception exception)
        {
            failure = exception;
            throw;
        }
        finally
        {
            if (_operations.TryRemove(new KeyValuePair<string, Operation>(operationKey, selected)))
            {
                var outcome = failure is null ? ActivationOutcome.Ready : Outcome(failure);
                var key = OutcomeKey(outcome);
                aggregate.Complete(pendingBytes, key);
                selected.Metrics.Complete(key);
                if (failure is not null)
                    TraceFailure(activation, payload, flow, outcome, failure);
            }
        }
    }

    internal ZLinkInstanceSpotOperationSnapshot Snapshot(string stableType) =>
        _aggregates.TryGetValue(stableType, out var aggregate) ? aggregate.Snapshot() : default;

    private static void TraceFailure(
        InstanceSpotActivationOperation activation,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        ZLinkMessageFlowTracer? flow,
        ActivationOutcome outcome,
        Exception error
    )
    {
        if (activation.IsRequest || flow is null || !flow.Enabled(ZLinkMessageFlowOutcome.Dropped))
            return;

        ZLinkEnvelopeHeader? header = null;
        if (flow.CaptureEnabled && payload.Count > 0)
        {
            try
            {
                header = ZLinkEnvelopeCodec.DecodeHeader(payload[0].Span, validateFlow: true);
            }
            catch (ZLinkEnvelopeProtocolException protocolError)
            {
                // Preserve readable correlation even when the first message is malformed.
                header = protocolError.Header;
            }
        }
        var pair = header is null ? default : ZLinkEnvelopeCodec.ValidFlow(header);
        var details = ZLinkTraceFormat.DispatchErrorDetails(error);
        flow.Trace(
            new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.Dropped,
                ZLinkDispatchErrorSurface.InstanceSpot,
                ZLinkDispatchMessageKind.Send,
                PacketName: header?.MessageName,
                CorrelationId: header?.CorrelationId,
                SourceRid: activation.SourceNodeRid.ToHex(),
                SpotId: activation.Target.TargetSpotId,
                MeshName: activation.Target.MeshName,
                InstanceSpotType: activation.Target.StableType,
                Reason: outcome switch
                {
                    ActivationOutcome.TimedOut => ZLinkMessageFlowReason.ActivationTimeout,
                    ActivationOutcome.Shutdown => ZLinkMessageFlowReason.Shutdown,
                    _ => ZLinkMessageFlowReason.ActivationRejected,
                }
            )
            {
                FlowId = pair.FlowId ?? string.Empty,
                FlowOrigin = pair.FlowOrigin,
                ErrorType = details.Type,
                ErrorMessage = details.Message,
            }
        );
    }

    private static ActivationOutcome Outcome(Exception exception) =>
        exception switch
        {
            TimeoutException => ActivationOutcome.TimedOut,
            OperationCanceledException => ActivationOutcome.Shutdown,
            ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.DeadlineExceeded } =>
                ActivationOutcome.TimedOut,
            ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.ShuttingDown } =>
                ActivationOutcome.Shutdown,
            ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.InvalidOperation } =>
                ActivationOutcome.Fenced,
            ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.AlreadyExists } =>
                ActivationOutcome.Conflict,
            ZLinkFrameworkException
            {
                Kind: ZLinkFrameworkErrorKind.InternalFailure or ZLinkFrameworkErrorKind.NotFound
            } => ActivationOutcome.StoreFailure,
            _ => ActivationOutcome.Rejected,
        };

    private sealed class Operation(
        string stableType,
        ZLinkRuntimeMetrics.ZLinkInstanceSpotMetricOperation metrics
    )
    {
        private int _accepted;

        internal string StableType { get; } = stableType;
        internal ZLinkRuntimeMetrics.ZLinkInstanceSpotMetricOperation Metrics { get; } = metrics;

        internal bool TryAccept() => Interlocked.Exchange(ref _accepted, 1) == 0;
    }

    private sealed class Aggregate
    {
        private readonly object _gate = new();
        private ulong _pendingMessages;
        private ulong _pendingBytes;
        private string? _lastOutcome;

        internal void Accept(ulong bytes)
        {
            lock (_gate)
            {
                _pendingMessages = checked(_pendingMessages + 1);
                _pendingBytes = checked(_pendingBytes + bytes);
            }
        }

        internal void Complete(ulong bytes, string outcome)
        {
            lock (_gate)
            {
                if (_pendingMessages == 0 || _pendingBytes < bytes)
                    throw new InvalidOperationException(
                        "Instance monitoring counters became inconsistent."
                    );
                _pendingMessages--;
                _pendingBytes -= bytes;
                _lastOutcome = outcome;
            }
        }

        internal ZLinkInstanceSpotOperationSnapshot Snapshot()
        {
            lock (_gate)
                return new ZLinkInstanceSpotOperationSnapshot(
                    _pendingMessages,
                    _pendingBytes,
                    _lastOutcome
                );
        }
    }
}

internal readonly record struct ZLinkInstanceSpotOperationSnapshot(
    ulong PendingMessageCount,
    ulong PendingByteCount,
    string? LastActivationOutcome
);

internal readonly record struct ZLinkInstanceSpotCatalogSnapshot(
    ulong ActiveCount,
    ulong ActivatingCount,
    ulong ClosingCount
);
