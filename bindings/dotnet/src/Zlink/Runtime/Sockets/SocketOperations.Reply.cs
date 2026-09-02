// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;

namespace Systems.Zlink;

internal abstract class RouterReplyOperation : ReplyOperation,
    ReplySubmitOperation
{
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public ReplySubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
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
    private readonly ReplyToken _replyToken;
    private readonly RouterSocket _socket;

    internal RouterPeerReplyOperation(
        RouterSocket socket,
        RoutingId peerRid,
        ReplyToken replyToken)
    {
        _socket = socket;
        _peerRid = peerRid;
        _replyToken = replyToken;
    }

    protected override void SubmitCore(IReadOnlyList<Message> parts)
    {
        _socket.ReplyCore(_peerRid, _replyToken, parts);
    }
}

internal sealed class ReceivedReplyOperationImpl : ReplyOperation,
    ReplySubmitOperation
{
    private readonly ReceivedReplyContext _replyContext;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal ReceivedReplyOperationImpl(ReceivedReplyContext replyContext)
    {
        _replyContext = replyContext;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public ReplySubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Submit()
    {
        EnsureReady();
        _replyContext.Reply(_parts.Parts);
        _submission.MarkSubmittedAfterValidation();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}
