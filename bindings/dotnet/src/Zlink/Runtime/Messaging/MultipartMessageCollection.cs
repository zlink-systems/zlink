// SPDX-License-Identifier: MPL-2.0

using System.Collections;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class MultipartMessageCollection : IReadOnlyList<Message>, IDisposable
{
    private readonly int _count;
    private Message[]? _messages;

    private MultipartMessageCollection(Message[] messages)
    {
        _messages = messages;
        _count = messages.Length;
    }

    internal bool IsSinglePart => Count == 1;

    public void Dispose()
    {
        var messages = _messages;
        if (messages == null)
            return;
        _messages = null;

        for (var i = 0; i < messages.Length; i++)
            messages[i].Dispose();
    }

    public int Count => _count;

    public Message this[int index]
    {
        get
        {
            var messages = GetMessages();
            if ((uint)index >= (uint)messages.Length)
                throw new ArgumentOutOfRangeException(nameof(index));
            return messages[index];
        }
    }

    public IEnumerator<Message> GetEnumerator()
    {
        var messages = GetMessages();
        for (var i = 0; i < messages.Length; i++)
            yield return messages[i];
    }

    IEnumerator IEnumerable.GetEnumerator()
    {
        return GetEnumerator();
    }

    internal static MultipartMessageCollection FromMessages(Message[] messages)
    {
        return new MultipartMessageCollection(messages ?? Array.Empty<Message>());
    }

    internal static MultipartMessageCollection FromSingle(Message message)
    {
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        return new MultipartMessageCollection(new[] { message });
    }

    internal static MultipartMessageCollection FromNativeSingle(ref ZlinkMsg nativePart)
    {
        var message = Message.AdoptNative(ref nativePart);
        try
        {
            return FromSingle(message);
        }
        catch
        {
            message.Dispose();
            throw;
        }
    }

    internal static MultipartMessageCollection FromNativeParts(ZlinkMsg[] nativeParts)
    {
        return FromNativeParts(nativeParts, nativeParts?.Length ?? 0);
    }

    internal static MultipartMessageCollection FromNativeParts(ZlinkMsg[] nativeParts,
        int nativePartCount)
    {
        if (nativeParts == null)
            return new MultipartMessageCollection(Array.Empty<Message>());
        if ((uint)nativePartCount > (uint)nativeParts.Length)
            throw new ArgumentOutOfRangeException(nameof(nativePartCount));
        var messages = new Message[nativePartCount];
        var built = 0;
        try
        {
            for (; built < nativePartCount; built++)
                messages[built] = Message.MoveFromNative(ref nativeParts[built]);
            return new MultipartMessageCollection(messages);
        }
        catch
        {
            for (var i = 0; i < built; i++)
                messages[i].Dispose();
            throw;
        }
    }

    internal Message[] TakeMessages()
    {
        var messages = _messages;
        if (messages == null)
            return Array.Empty<Message>();
        _messages = null;
        return messages;
    }

    private Message[] GetMessages()
    {
        return _messages
               ?? throw new ObjectDisposedException(nameof(MultipartMessageCollection));
    }
}
