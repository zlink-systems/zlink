// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal delegate void ReceivedReplyHandler(IReadOnlyList<Message> parts);

internal delegate bool ReceivedSendSingleHandler(Message part, SendFlags flags);

internal delegate bool ReceivedSendHandler(IReadOnlyList<Message> parts,
    SendFlags flags);
