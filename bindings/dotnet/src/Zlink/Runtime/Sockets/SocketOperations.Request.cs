// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;

namespace Systems.Zlink;

internal sealed class DealerRequestOperation : RequestOperation,
    RequestSubmitOperation
{
    private readonly DealerSocket _socket;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;
    private TimeSpan _timeout;

    internal DealerRequestOperation(DealerSocket socket)
    {
        _socket = socket;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RequestSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RequestSubmitOperation Timeout(TimeSpan timeout)
    {
        EnsureNotSubmitted();
        _timeout = timeout;
        return this;
    }

    public Task<IReadOnlyList<Message>> Async(
        CancellationToken ct = default)
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
        _submission.MarkSubmittedAfterValidation();
        return _socket.RequestCore(_parts.Parts, _timeout, ct);
    }

    public IReadOnlyList<Message> Submit(SendFlags flags)
    {
        EnsureReadyToSubmit();
        return _socket.RequestCore(_parts.Parts, _timeout, flags);
    }

    public void Submit(SendFlags flags, RequestCallback callback)
    {
        EnsureReadyToSubmit();
        _socket.RequestCore(_parts.Parts, _timeout, flags, callback);
    }

    private void EnsureReadyToSubmit()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
        _submission.MarkSubmittedAfterValidation();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}

internal sealed class RouterPeerRequestOperation : RequestOperation,
    RequestSubmitOperation
{
    private readonly RoutingId _peerRid;
    private readonly RouterSocket _socket;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;
    private TimeSpan _timeout;

    internal RouterPeerRequestOperation(RouterSocket socket,
        RoutingId peerRid)
    {
        _socket = socket;
        _peerRid = peerRid;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RequestSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RequestSubmitOperation Timeout(TimeSpan timeout)
    {
        EnsureNotSubmitted();
        _timeout = timeout;
        return this;
    }

    public Task<IReadOnlyList<Message>> Async(
        CancellationToken ct = default)
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
        _submission.MarkSubmittedAfterValidation();
        return _socket.RequestCore(_peerRid, _parts.Parts, _timeout, ct);
    }

    public IReadOnlyList<Message> Submit(SendFlags flags)
    {
        EnsureReadyToSubmit();
        return _socket.RequestCore(_peerRid, _parts.Parts, _timeout, flags);
    }

    public void Submit(SendFlags flags, RequestCallback callback)
    {
        EnsureReadyToSubmit();
        _socket.RequestCore(_peerRid, _parts.Parts, _timeout, flags, callback);
    }

    private void EnsureReadyToSubmit()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
        _submission.MarkSubmittedAfterValidation();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}
