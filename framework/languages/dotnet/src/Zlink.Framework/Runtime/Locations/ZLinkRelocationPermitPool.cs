namespace Zlink.Framework.Runtime.Locations;

internal readonly record struct ZLinkRelocationPermitRequest(
    int OutboundUnits,
    int InboundUnits,
    int CaptureCallbacks,
    int RestoreCallbacks,
    long PayloadBytes,
    bool AllowOversizedPayload = false)
{
    internal static ZLinkRelocationPermitRequest OutboundUnit() =>
        new(
            OutboundUnits: 1,
            InboundUnits: 0,
            CaptureCallbacks: 0,
            RestoreCallbacks: 0,
            PayloadBytes: 0);

    internal static ZLinkRelocationPermitRequest Capture() =>
        new(
            OutboundUnits: 0,
            InboundUnits: 0,
            CaptureCallbacks: 1,
            RestoreCallbacks: 0,
            PayloadBytes: 0);

    internal static ZLinkRelocationPermitRequest Payload(long payloadBytes) =>
        new(
            OutboundUnits: 0,
            InboundUnits: 0,
            CaptureCallbacks: 0,
            RestoreCallbacks: 0,
            PayloadBytes: payloadBytes);

    internal static ZLinkRelocationPermitRequest Outbound(
        long payloadBytes,
        bool capture,
        bool allowOversizedPayload = false) =>
        new(
            OutboundUnits: 1,
            InboundUnits: 0,
            CaptureCallbacks: capture ? 1 : 0,
            RestoreCallbacks: 0,
            PayloadBytes: payloadBytes,
            AllowOversizedPayload: allowOversizedPayload);

    internal static ZLinkRelocationPermitRequest Inbound(
        long payloadBytes,
        bool restore,
        bool allowOversizedPayload = false) =>
        new(
            OutboundUnits: 0,
            InboundUnits: 1,
            CaptureCallbacks: 0,
            RestoreCallbacks: restore ? 1 : 0,
            PayloadBytes: payloadBytes,
            AllowOversizedPayload: allowOversizedPayload);
}

/// <summary>
/// Owns process-wide relocation admission as one atomic accounting domain.
/// A caller either receives every requested permit or leaves all counters unchanged.
/// </summary>
internal sealed class ZLinkRelocationPermitPool
{
    private readonly object _gate = new();
    private readonly ZLinkLocationOptions _options;
    private int _outboundUnits;
    private int _inboundUnits;
    private int _captureCallbacks;
    private int _restoreCallbacks;
    private long _payloadBytes;
    private bool _oversizedPayloadActive;

    internal ZLinkRelocationPermitPool(ZLinkLocationOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        _options = options;
        _ = ReadLimits();
    }

    internal bool TryAcquire(
        ZLinkRelocationPermitRequest request,
        out ZLinkRelocationPermitLease lease)
    {
        Validate(request);
        lock (_gate)
        {
            var limits = ReadLimits();
            var oversized = request.PayloadBytes > limits.MaxPayloadBytes;
            if (_outboundUnits > limits.MaxOutboundUnits - request.OutboundUnits
                || _inboundUnits > limits.MaxInboundUnits - request.InboundUnits
                || _captureCallbacks > limits.MaxCaptureCallbacks - request.CaptureCallbacks
                || _restoreCallbacks > limits.MaxRestoreCallbacks - request.RestoreCallbacks
                || !CanAdmitPayload(request, oversized, limits.MaxPayloadBytes))
            {
                lease = default;
                return false;
            }

            _outboundUnits += request.OutboundUnits;
            _inboundUnits += request.InboundUnits;
            _captureCallbacks += request.CaptureCallbacks;
            _restoreCallbacks += request.RestoreCallbacks;
            _payloadBytes = checked(_payloadBytes + request.PayloadBytes);
            _oversizedPayloadActive = oversized;
            lease = new ZLinkRelocationPermitLease(this, request, oversized);
            return true;
        }
    }

    internal bool CanAcquire(ZLinkRelocationPermitRequest request)
    {
        Validate(request);
        lock (_gate)
        {
            var limits = ReadLimits();
            var oversized = request.PayloadBytes > limits.MaxPayloadBytes;
            return _outboundUnits <= limits.MaxOutboundUnits - request.OutboundUnits
                && _inboundUnits <= limits.MaxInboundUnits - request.InboundUnits
                && _captureCallbacks <= limits.MaxCaptureCallbacks
                    - request.CaptureCallbacks
                && _restoreCallbacks <= limits.MaxRestoreCallbacks
                    - request.RestoreCallbacks
                && CanAdmitPayload(request, oversized, limits.MaxPayloadBytes);
        }
    }

    internal bool TryGetInboundOffer(out ulong messages, out ulong bytes)
    {
        lock (_gate)
        {
            var limits = ReadLimits();
            var remainingBytes = limits.MaxPayloadBytes - _payloadBytes;
            if (_oversizedPayloadActive
                || _inboundUnits >= limits.MaxInboundUnits
                || _restoreCallbacks >= limits.MaxRestoreCallbacks
                || remainingBytes <= 0)
            {
                messages = 0;
                bytes = 0;
                return false;
            }
            bytes = checked((ulong)remainingBytes);
            // Every frozen record occupies at least one encoded byte. This is
            // a conservative message allowance from the same actual budget.
            messages = bytes;
            return true;
        }
    }

    internal ZLinkRelocationPermitSnapshot Snapshot()
    {
        lock (_gate)
            return new ZLinkRelocationPermitSnapshot(
                _outboundUnits,
                _inboundUnits,
                _captureCallbacks,
                _restoreCallbacks,
                _payloadBytes,
                _oversizedPayloadActive);
    }

    private bool CanAdmitPayload(
        ZLinkRelocationPermitRequest request,
        bool oversized,
        long maxPayloadBytes)
    {
        if (oversized)
            return request.AllowOversizedPayload
                   && !_oversizedPayloadActive
                   && _payloadBytes == 0;
        return !_oversizedPayloadActive
               && _payloadBytes <= maxPayloadBytes - request.PayloadBytes;
    }

    private Limits ReadLimits() => new(
        Positive(
            _options.MaxActiveOutboundRelocations,
            nameof(_options.MaxActiveOutboundRelocations)),
        Positive(
            _options.MaxActiveInboundRelocations,
            nameof(_options.MaxActiveInboundRelocations)),
        Positive(
            _options.MaxConcurrentRelocationCaptures,
            nameof(_options.MaxConcurrentRelocationCaptures)),
        Positive(
            _options.MaxConcurrentRelocationRestores,
            nameof(_options.MaxConcurrentRelocationRestores)),
        Positive(
            _options.MaxRelocationPayloadInFlightBytes,
            nameof(_options.MaxRelocationPayloadInFlightBytes)));

    private bool TryShrinkPayload(
        ref ZLinkRelocationPermitRequest request,
        long actualPayloadBytes)
    {
        if (actualPayloadBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(actualPayloadBytes));
        lock (_gate)
        {
            if (actualPayloadBytes > request.PayloadBytes)
                return false;
            _payloadBytes -= request.PayloadBytes - actualPayloadBytes;
            request = request with { PayloadBytes = actualPayloadBytes };
            return true;
        }
    }

    private void Release(
        ZLinkRelocationPermitRequest request,
        bool oversizedLease)
    {
        lock (_gate)
        {
            _outboundUnits -= request.OutboundUnits;
            _inboundUnits -= request.InboundUnits;
            _captureCallbacks -= request.CaptureCallbacks;
            _restoreCallbacks -= request.RestoreCallbacks;
            _payloadBytes -= request.PayloadBytes;
            if (oversizedLease)
                _oversizedPayloadActive = false;

            if (_outboundUnits < 0
                || _inboundUnits < 0
                || _captureCallbacks < 0
                || _restoreCallbacks < 0
                || _payloadBytes < 0)
                throw new InvalidOperationException(
                    "Relocation permit accounting became negative.");
        }
    }

    private static void Validate(ZLinkRelocationPermitRequest request)
    {
        if (request.OutboundUnits < 0
            || request.InboundUnits < 0
            || request.CaptureCallbacks < 0
            || request.RestoreCallbacks < 0
            || request.PayloadBytes < 0)
            throw new ArgumentOutOfRangeException(
                nameof(request),
                "Relocation permit counts and bytes cannot be negative.");
        if (request.OutboundUnits == 0
            && request.InboundUnits == 0
            && request.CaptureCallbacks == 0
            && request.RestoreCallbacks == 0
            && request.PayloadBytes == 0)
            throw new ArgumentException(
                "A relocation permit request must reserve at least one resource.",
                nameof(request));
    }

    private static int Positive(int value, string name) =>
        value > 0
            ? value
            : throw new ArgumentOutOfRangeException(name, value, "The limit must be greater than zero.");

    private static long Positive(long value, string name) =>
        value > 0
            ? value
            : throw new ArgumentOutOfRangeException(name, value, "The limit must be greater than zero.");

    private readonly record struct Limits(
        int MaxOutboundUnits,
        int MaxInboundUnits,
        int MaxCaptureCallbacks,
        int MaxRestoreCallbacks,
        long MaxPayloadBytes);

    internal readonly struct ZLinkRelocationPermitLease : IDisposable
    {
        private readonly LeaseState? _state;

        internal ZLinkRelocationPermitLease(
            ZLinkRelocationPermitPool owner,
            ZLinkRelocationPermitRequest request,
            bool oversized)
        {
            _state = new LeaseState(owner, request, oversized);
        }

        internal long ReservedPayloadBytes => _state?.ReservedPayloadBytes ?? 0;

        internal bool TryShrinkPayload(long actualPayloadBytes) =>
            _state?.TryShrinkPayload(actualPayloadBytes) ?? false;

        public void Dispose() => _state?.Dispose();

        private sealed class LeaseState : IDisposable
        {
            private readonly object _gate = new();
            private readonly ZLinkRelocationPermitPool _owner;
            private readonly bool _oversized;
            private ZLinkRelocationPermitRequest _request;
            private bool _disposed;

            internal LeaseState(
                ZLinkRelocationPermitPool owner,
                ZLinkRelocationPermitRequest request,
                bool oversized)
            {
                _owner = owner;
                _request = request;
                _oversized = oversized;
            }

            internal long ReservedPayloadBytes
            {
                get
                {
                    lock (_gate) return _disposed ? 0 : _request.PayloadBytes;
                }
            }

            internal bool TryShrinkPayload(long actualPayloadBytes)
            {
                lock (_gate)
                {
                    if (_disposed) return false;
                    return _owner.TryShrinkPayload(
                        ref _request,
                        actualPayloadBytes);
                }
            }

            public void Dispose()
            {
                lock (_gate)
                {
                    if (_disposed) return;
                    _disposed = true;
                    _owner.Release(_request, _oversized);
                }
            }
        }
    }
}

internal readonly record struct ZLinkRelocationPermitSnapshot(
    int OutboundUnits,
    int InboundUnits,
    int CaptureCallbacks,
    int RestoreCallbacks,
    long PayloadBytes,
    bool OversizedPayloadActive);
