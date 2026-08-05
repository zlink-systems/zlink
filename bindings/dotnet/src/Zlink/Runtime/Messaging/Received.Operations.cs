// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

// Continuation of Received; the type summary lives on the primary partial in
// Received.cs.
public sealed partial class Received : IDisposable
{
    internal ReceivedReplyHandler CaptureReplyHandler()
    {
        if (_metadata is not
            {
                RequestSeq: not null,
                ReplyHandler: { } replyHandler
            })
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        return replyHandler;
    }

    internal bool SendCore(Message part, SendFlags flags = SendFlags.None)
    {
        if (part == null)
            throw new ArgumentNullException(nameof(part));
        if (_sendKernel != null)
            return _sendKernel.SendReceivedSingle(_sendRoutingIdSnapshot,
                part, flags);
        if (_sendSingleHandler != null)
            return _sendSingleHandler(part, flags);
        return SendCore(new SingleMessageReadOnlyList(part), flags);
    }

    internal bool SendCore(IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        if (parts == null)
            throw new ArgumentNullException(nameof(parts));
        if (parts.Count == 1)
            return SendCore(parts[0], flags);
        if (_sendKernel != null)
            return _sendKernel.SendReceivedParts(_sendRoutingIdSnapshot,
                parts, flags);
        if (_sendHandler == null)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);

        return _sendHandler(parts, flags);
    }
}
