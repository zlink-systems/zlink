using System.Text.Json;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamAdmissionQueue(ZlinkStreamConnectorCallbacks callbacks)
{
    internal const string RequiredKey = "$zlink.admission.queueRequired";
    internal const string EnqueueName = ZlinkStreamHeaderCodec.AdmissionEnqueueName;
    internal const string PositionName = ZlinkStreamHeaderCodec.AdmissionPositionName;
    internal const string TicketName = ZlinkStreamHeaderCodec.AdmissionTicketName;
    private static readonly TimeSpan ReregistrationSilence = TimeSpan.FromSeconds(15);

    private readonly object _gate = new();
    private readonly LinkedList<Waiter> _waiters = new();
    private TaskCompletionSource<bool> _changed = NewSignal();
    private ZlinkStreamPendingRequests.PendingRequest? _registration;
    private Task<ZlinkStreamError>? _connectionEnded;
    private TimeSpan _registerAt;
    private string? _ticket;
    private long _ticketVersion;
    private bool _entered;

    internal static bool IsPush(string name) => name is PositionName or TicketName;

    internal long TicketVersion => Volatile.Read(ref _ticketVersion);

    internal bool HasWaiters
    {
        get
        {
            lock (_gate)
                return _waiters.Count != 0 && _ticket is null;
        }
    }

    internal void Reset()
    {
        lock (_gate)
        {
            if (_connectionEnded is not { IsCompleted: false })
                Clear();
        }
    }

    private void Clear()
    {
        _waiters.Clear();
        _registration = null;
        _connectionEnded = null;
        _ticket = null;
        _entered = false;
        Signal();
    }

    internal sealed class Waiter
    {
        internal string? Ticket { get; set; }
        internal ZlinkStreamError? Error { get; set; }
    }

    internal readonly record struct Work(
        string? Ticket,
        Task Changed,
        TimeSpan? RegisterAt,
        bool NotifyQueued
    );

    internal Waiter Join(
        Task<ZlinkStreamError> connection,
        long sentTicketVersion,
        string? rejectedTicket,
        TimeSpan now
    )
    {
        lock (_gate)
        {
            if (connection.IsCompleted)
                throw new ZlinkStreamException(connection.GetAwaiter().GetResult());

            if (!ReferenceEquals(_connectionEnded, connection))
            {
                Clear();
                _connectionEnded = connection;
            }

            var waiter = new Waiter();
            // Another rejection on this connection may already have obtained a newer ticket.
            if (
                _ticket is not null
                && rejectedTicket != _ticket
                && sentTicketVersion != _ticketVersion
            )
            {
                waiter.Ticket = _ticket;
                return waiter;
            }

            if (_waiters.Count == 0 || _ticket is not null)
            {
                _ticket = null;
                _registerAt = now;
                _entered = false;
            }

            _waiters.AddLast(waiter);
            return waiter;
        }
    }

    internal Work Next(Waiter waiter, TimeSpan now, out bool register)
    {
        lock (_gate)
        {
            if (waiter.Error is { } error)
                throw new ZlinkStreamException(error);

            var due =
                ReferenceEquals(_waiters.First?.Value, waiter) && waiter.Ticket is null
                    ? _registerAt
                    : (TimeSpan?)null;
            register = due <= now;
            var notify = register && !_entered;
            if (register)
                _entered = true;
            return new Work(waiter.Ticket, _changed.Task, due, notify);
        }
    }

    internal bool TryRegister(
        Waiter waiter,
        ZlinkStreamPendingRequests.PendingRequest request,
        TimeSpan now,
        out ZlinkStreamPendingRequests.PendingRequest? previous
    )
    {
        lock (_gate)
        {
            previous = null;
            if (
                !ReferenceEquals(_waiters.First?.Value, waiter)
                || waiter.Ticket is not null
                || waiter.Error is not null
                || now < _registerAt
            )
                return false;

            previous = _registration;
            _registration = request;
            _registerAt = now + ReregistrationSilence;
            return true;
        }
    }

    internal void Abandon(ZlinkStreamPendingRequests.PendingRequest request, TimeSpan now)
    {
        lock (_gate)
        {
            if (!ReferenceEquals(_registration, request))
                return;
            _registration = null;
            _registerAt = now;
            Signal();
        }
    }

    internal ZlinkStreamPendingRequests.PendingRequest? Leave(Waiter waiter)
    {
        lock (_gate)
        {
            if (!_waiters.Remove(waiter))
                return null;
            Signal();
            if (_waiters.Count != 0)
                return null;
            var registration = _registration;
            _registration = null;
            return registration;
        }
    }

    internal ValueTask OnResponseAsync(
        ZlinkStreamPendingRequests.PendingRequest registration,
        Waiter owner,
        ZlinkStreamPendingCompletion completion
    )
    {
        var update = completion.Error is null
            ? Decode(completion.Header.Codec, completion.Frame.Payload, null)
            : null;
        lock (_gate)
        {
            if (!ReferenceEquals(_registration, registration))
                return default;
            _registration = null;

            if (update is null)
            {
                owner.Error = completion.Error ?? DecodeError();
                _waiters.Remove(owner);
                Signal();
                return default;
            }

            if (_waiters.Count == 0 || _ticket is not null)
                return default;
            Apply(update.Value);
        }

        return NotifyPositionAsync(update.Value);
    }

    internal ValueTask OnSendAsync(
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload,
        TimeSpan receivedAt
    )
    {
        var update = Decode(header.Codec, payload, header.Name);
        lock (_gate)
        {
            if (_waiters.Count == 0 || _ticket is not null)
                return default;

            if (update is null)
            {
                var error = DecodeError();
                foreach (var waiter in _waiters)
                    waiter.Error = error;
                Signal();
                throw new ZlinkStreamException(error);
            }

            _registerAt = receivedAt + ReregistrationSilence;
            Apply(update.Value);
        }

        return NotifyPositionAsync(update.Value);
    }

    private void Apply(Update update)
    {
        if (update.Ticket is { } ticket)
        {
            _ticket = ticket;
            Interlocked.Increment(ref _ticketVersion);
            foreach (var waiter in _waiters)
                waiter.Ticket = ticket;
        }
        Signal();
    }

    private ValueTask NotifyPositionAsync(Update update) =>
        update.Position is { } position
            ? callbacks.NotifyAdmissionPositionAsync(position)
            : default;

    private void Signal()
    {
        var previous = _changed;
        _changed = NewSignal();
        previous.TrySetResult(true);
    }

    private static TaskCompletionSource<bool> NewSignal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private static ZlinkStreamError DecodeError() =>
        new(
            ZlinkStreamErrorCode.FrameDecodeFailed,
            "Invalid authentication admission queue packet."
        );

    private readonly record struct Update(string? Ticket, ZlinkStreamAdmissionPosition? Position);

    private static Update? Decode(
        ZlinkStreamCodec codec,
        ReadOnlyMemory<byte> payload,
        string? packet
    )
    {
        if (codec != ZlinkStreamCodec.Json)
            return null;

        try
        {
            using var document = JsonDocument.Parse(payload);
            var body = document.RootElement;
            if (body.ValueKind != JsonValueKind.Object || body.EnumerateObject().Count() != 2)
                return null;

            if (
                packet != PositionName
                && body.TryGetProperty("ticket", out var ticket)
                && ticket.ValueKind == JsonValueKind.String
                && ticket.GetString() is { } value
                && value.Length == ZlinkAdmissionTicketWire.EncodedSize
                && value.All(IsBase64Url)
                && ZlinkAdmissionTicketWire.CanonicalFinalCharacters.Contains(value[^1])
                && TryReadInt(body, "expiresInMs", out var expiresInMs)
                && expiresInMs > 0
            )
                return new Update(value, null);

            if (
                packet != TicketName
                && TryReadInt(body, "position", out var position)
                && position > 0
                && TryReadInt(body, "estimatedWaitMs", out var estimatedWaitMs)
                && estimatedWaitMs >= 0
            )
                return new Update(
                    null,
                    new ZlinkStreamAdmissionPosition(position, estimatedWaitMs)
                );
        }
        catch (JsonException)
        {
            return null;
        }
        return null;
    }

    private static bool IsBase64Url(char value) =>
        value is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '-' or '_';

    private static bool TryReadInt(JsonElement body, string name, out int value)
    {
        value = default;
        return body.TryGetProperty(name, out var element)
            && element.ValueKind == JsonValueKind.Number
            && element.TryGetInt32(out value);
    }
}
