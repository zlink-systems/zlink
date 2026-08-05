// SPDX-License-Identifier: MPL-2.0

using System.Collections;

namespace Systems.Zlink;

internal struct OperationMessageBuffer
{
    // Hot path: framework/raw request messages are commonly header + body.
    // Keep the first two parts in fields and allocate List<Message> only when
    // callers build a larger multipart message.
    private Message? _singlePart;
    private Message? _secondPart;
    private List<Message>? _parts;

    internal int Count =>
        _parts?.Count ?? (_singlePart == null ? 0 : _secondPart == null ? 1 : 2);

    internal bool IsSingle => _singlePart != null && _secondPart == null;

    internal Message Single => _singlePart
                               ?? throw new ZlinkConfigException(
                                   ZlinkConfigException.ErrorCode.InvalidState);

    internal IReadOnlyList<Message> Parts
    {
        get
        {
            if (_parts != null)
                return _parts;
            if (_singlePart == null)
                throw new ZlinkConfigException(
                    ZlinkConfigException.ErrorCode.InvalidState);
            return _secondPart == null
                ? new SingleMessageReadOnlyList(_singlePart)
                : new TwoMessageReadOnlyList(_singlePart, _secondPart);
        }
    }

    internal IReadOnlyList<Message> PartsOrEmpty =>
        Count == 0 ? Array.Empty<Message>() : Parts;

    internal void Add(Message message)
    {
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        if (_parts != null)
        {
            _parts.Add(message);
            return;
        }

        if (_singlePart == null)
        {
            _singlePart = message;
            return;
        }

        if (_secondPart == null)
        {
            _secondPart = message;
            return;
        }

        _parts = new List<Message> { _singlePart, _secondPart, message };
        _singlePart = null;
        _secondPart = null;
    }

    internal void EnsureNotEmpty()
    {
        if (Count == 0)
            throw new ZlinkConfigException(
                ZlinkConfigException.ErrorCode.InvalidArgument);
    }

    private sealed class TwoMessageReadOnlyList : IReadOnlyList<Message>
    {
        private readonly Message _first;
        private readonly Message _second;

        internal TwoMessageReadOnlyList(Message first, Message second)
        {
            _first = first;
            _second = second;
        }

        public int Count => 2;

        public Message this[int index]
        {
            get
            {
                return index switch
                {
                    0 => _first,
                    1 => _second,
                    _ => throw new ArgumentOutOfRangeException(nameof(index))
                };
            }
        }

        public IEnumerator<Message> GetEnumerator()
        {
            yield return _first;
            yield return _second;
        }

        IEnumerator IEnumerable.GetEnumerator()
        {
            return GetEnumerator();
        }
    }
}
