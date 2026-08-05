namespace Zlink.Framework.Runtime.Spots;

internal enum ZLinkRelocationReplyAckState : byte
{
    NotAcknowledged = 0,
    TerminalReceived = 1,
    AlreadyTerminal = 2
}
