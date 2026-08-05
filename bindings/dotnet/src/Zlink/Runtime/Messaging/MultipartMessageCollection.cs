// SPDX-License-Identifier: MPL-2.0

using System.Collections;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class MultipartMessageCollection : IReadOnlyList<Message>, IDisposable
{
    private readonly object _gate = new();
    private readonly Message?[] _messages;
    private int _closed;
    private int _nativePartCount;
    private ZlinkMsg[]? _nativeParts;

    private MultipartMessageCollection(Message[] messages)
    {
        _messages = new Message?[messages.Length];
        for (var i = 0; i < messages.Length; i++)
            _messages[i] = messages[i];
    }

    private MultipartMessageCollection(ZlinkMsg[] nativeParts)
        : this(nativeParts, nativeParts.Length)
    {
    }

    private MultipartMessageCollection(ZlinkMsg[] nativeParts, int nativePartCount)
    {
        _messages = new Message?[nativePartCount];
        _nativeParts = nativeParts;
        _nativePartCount = nativePartCount;
    }

    internal bool IsSinglePart => Count == 1;

    private bool IsDisposed => Volatile.Read(ref _closed) != 0;

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0)
            return;

        lock (_gate)
        {
            for (var i = 0; i < _messages.Length; i++)
                _messages[i]?.Dispose();

            if (_nativeParts == null)
                return;

            for (var i = 0; i < _nativePartCount; i++)
                NativeMethods.zlink_msg_close(ref _nativeParts[i]);
            _nativeParts = null;
            _nativePartCount = 0;
        }
    }

    public int Count => _messages.Length;

    public Message this[int index]
    {
        get
        {
            if (IsDisposed)
                throw new ObjectDisposedException(nameof(MultipartMessageCollection));
            if ((uint)index >= (uint)_messages.Length)
                throw new ArgumentOutOfRangeException(nameof(index));
            return GetOrMaterialize(index);
        }
    }

    public IEnumerator<Message> GetEnumerator()
    {
        if (IsDisposed)
            throw new ObjectDisposedException(nameof(MultipartMessageCollection));
        for (var i = 0; i < _messages.Length; i++)
            yield return GetOrMaterialize(i);
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
        return new MultipartMessageCollection(nativeParts ?? Array.Empty<ZlinkMsg>());
    }

    internal static MultipartMessageCollection FromNativeParts(ZlinkMsg[] nativeParts,
        int nativePartCount)
    {
        if (nativeParts == null)
            return new MultipartMessageCollection(Array.Empty<ZlinkMsg>(), 0);
        if ((uint)nativePartCount > (uint)nativeParts.Length)
            throw new ArgumentOutOfRangeException(nameof(nativePartCount));
        return new MultipartMessageCollection(nativeParts, nativePartCount);
    }

    internal Message[] TakeMessages()
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0)
            return Array.Empty<Message>();

        lock (_gate)
        {
            var taken = new Message[_messages.Length];
            for (var i = 0; i < _messages.Length; i++)
            {
                taken[i] = GetOrMaterialize(i);
                _messages[i] = null;
            }

            _nativeParts = null;
            _nativePartCount = 0;
            return taken;
        }
    }

    private Message GetOrMaterialize(int index)
    {
        var existing = Volatile.Read(ref _messages[index]);
        if (existing != null)
            return existing;

        lock (_gate)
        {
            existing = _messages[index];
            if (existing != null)
                return existing;

            if (_nativeParts == null)
                throw new ObjectDisposedException(nameof(MultipartMessageCollection));

            var created = Message.MoveFromNative(ref _nativeParts[index]);
            _messages[index] = created;
            if (AllMaterialized())
            {
                _nativeParts = null;
                _nativePartCount = 0;
            }

            return created;
        }
    }

    private bool AllMaterialized()
    {
        for (var i = 0; i < _messages.Length; i++)
            if (_messages[i] == null)
                return false;

        return true;
    }
}
