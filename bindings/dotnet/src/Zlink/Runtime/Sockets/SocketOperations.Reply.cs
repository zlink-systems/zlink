// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal abstract class RouterReplyOperation : ReplyOperation,
    ReplySubmitOperation
{
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    public ReplySubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    public void Submit()
    {
        EnsureReady();
        SubmitCore(_parts.Parts);
        _submission.MarkSubmittedAfterValidation();
    }

    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }

    protected abstract void SubmitCore(IReadOnlyList<Message> parts);
}

internal sealed class RouterPeerReplyOperation : RouterReplyOperation
{
    private readonly RoutingId _peerRid;
    private readonly ulong _requestSeq;
    private readonly RouterSocket _socket;

    internal RouterPeerReplyOperation(
        RouterSocket socket,
        RoutingId peerRid,
        ulong requestSeq)
    {
        _socket = socket;
        _peerRid = peerRid;
        _requestSeq = requestSeq;
    }

    protected override void SubmitCore(IReadOnlyList<Message> parts)
    {
        _socket.ReplyCore(_peerRid, _requestSeq, parts);
    }
}

internal sealed class ReceivedReplyOperationImpl : ReplyOperation,
    ReplySubmitOperation
{
    private readonly ReceivedReplyHandler _replyHandler;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal ReceivedReplyOperationImpl(ReceivedReplyHandler replyHandler)
    {
        _replyHandler = replyHandler;
    }

    public ReplySubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    public void Submit()
    {
        EnsureReady();
        _replyHandler(_parts.Parts);
        _submission.MarkSubmittedAfterValidation();
    }

    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}
