using System.Collections;

namespace Zlink.Framework.Runtime.Messaging;

internal interface IZLinkApplicationPayloadSized
{
    ulong ApplicationPayloadBytes { get; }
}

internal static class ZLinkMessageParts
{
    public static IReadOnlyList<Message> Create(
        Message header,
        Message body)
    {
        // Hot path: most envelopes are exactly header + body. Keep this as a
        // tiny IReadOnlyList wrapper so send/request avoids allocating arrays.
        return new TwoMessageParts(header, body);
    }

    public static void DisposeAll(IReadOnlyList<Message> parts)
    {
        for (var index = 0; index < parts.Count; index++) parts[index].Dispose();
    }

    public static IReadOnlyList<Message> CopyAll(IReadOnlyList<Message> parts)
    {
        var copies = new Message[parts.Count];
        var copied = 0;
        try
        {
            for (; copied < parts.Count; copied++) copies[copied] = parts[copied].Copy();

            return parts is IZLinkApplicationPayloadSized sized
                ? new KnownMessageParts(copies, sized.ApplicationPayloadBytes)
                : copies;
        }
        catch
        {
            for (var index = 0; index < copied; index++) copies[index].Dispose();

            throw;
        }
    }

    public static IReadOnlyList<Message> WithApplicationPayloadBytes(
        Message[] parts,
        ulong applicationPayloadBytes) =>
        new KnownMessageParts(parts, applicationPayloadBytes);

    private sealed class TwoMessageParts(
        Message header,
        Message body) : IReadOnlyList<Message>, IZLinkApplicationPayloadSized
    {
        public int Count => 2;

        public ulong ApplicationPayloadBytes =>
            checked((ulong)Math.Max(body.Size, 0));

        public Message this[int index]
        {
            get
            {
                return index switch
                {
                    0 => header,
                    1 => body,
                    _ => throw new ArgumentOutOfRangeException(nameof(index))
                };
            }
        }

        public IEnumerator<Message> GetEnumerator()
        {
            yield return header;
            yield return body;
        }

        IEnumerator IEnumerable.GetEnumerator()
        {
            return GetEnumerator();
        }
    }

    private sealed class KnownMessageParts(
        Message[] parts,
        ulong applicationPayloadBytes) : IReadOnlyList<Message>, IZLinkApplicationPayloadSized
    {
        public int Count => parts.Length;

        public ulong ApplicationPayloadBytes => applicationPayloadBytes;

        public Message this[int index] => parts[index];

        public IEnumerator<Message> GetEnumerator() =>
            ((IEnumerable<Message>)parts).GetEnumerator();

        IEnumerator IEnumerable.GetEnumerator() => parts.GetEnumerator();
    }
}
