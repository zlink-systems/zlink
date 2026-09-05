// SPDX-License-Identifier: MPL-2.0

using System.Collections;
using System.Buffers;

namespace Systems.Zlink;

internal sealed class MultipartMessageCollection : IReadOnlyList<Message>, IDisposable
{
    private readonly int _count;
    private Message[]? _messages;
    private readonly bool _rented;

    private MultipartMessageCollection(Message[] messages, int count = -1,
        bool rented = false)
    {
        _messages = messages;
        _count = count < 0 ? messages.Length : count;
        _rented = rented;
    }

    internal bool IsSinglePart => Count == 1;

    public void Dispose()
    {
        var messages = _messages;
        if (messages == null)
            return;
        _messages = null;

        for (var i = 0; i < _count; i++)
            messages[i].Dispose();
        ReturnStorage(messages);
    }

    public int Count => _count;

    public Message this[int index]
    {
        get
        {
            var messages = GetMessages();
            if ((uint)index >= (uint)_count)
                throw new ArgumentOutOfRangeException(nameof(index));
            return messages[index];
        }
    }

    public IEnumerator<Message> GetEnumerator()
    {
        _ = GetMessages();
        for (var i = 0; i < _count; i++)
            yield return this[i];
    }

    IEnumerator IEnumerable.GetEnumerator()
    {
        return GetEnumerator();
    }

    internal static MultipartMessageCollection FromMessages(Message[] messages,
        int count = -1, bool rented = false)
    {
        return new MultipartMessageCollection(messages ?? Array.Empty<Message>(),
            count, rented);
    }

    internal static MultipartMessageCollection FromSingle(Message message)
    {
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        return new MultipartMessageCollection(new[] { message });
    }

    internal Message[] TakeMessages()
    {
        var messages = _messages;
        if (messages == null)
            return Array.Empty<Message>();
        // Public ownership transfer needs independent, exact-length storage.
        // The collection's private pooled buffer cannot escape its lifetime.
        var result = _rented ? messages.AsSpan(0, _count).ToArray() : messages;
        _messages = null;
        ReturnStorage(messages);
        return result;
    }

    private void ReturnStorage(Message[] messages)
    {
        if (_rented)
            ArrayPool<Message>.Shared.Return(messages, clearArray: true);
    }

    private Message[] GetMessages()
    {
        return _messages
               ?? throw new ObjectDisposedException(nameof(MultipartMessageCollection));
    }
}
