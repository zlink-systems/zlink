// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal static class MessageEnvelopeParts
{
    internal static Message First(IReadOnlyList<Message> parts,
        string envelopeName)
    {
        return parts.Count > 0
            ? parts[0]
            : throw new InvalidOperationException(
                $"{envelopeName} does not contain any message parts.");
    }

    internal static Message Single(IReadOnlyList<Message> parts,
        string envelopeName)
    {
        return parts.Count == 1
            ? parts[0]
            : throw new InvalidOperationException(
                $"{envelopeName} does not contain exactly one message part.");
    }
}
