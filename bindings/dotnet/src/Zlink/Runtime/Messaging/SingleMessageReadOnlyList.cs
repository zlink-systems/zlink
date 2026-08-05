// SPDX-License-Identifier: MPL-2.0

using System.Collections;

namespace Systems.Zlink;

internal sealed class SingleMessageReadOnlyList : IReadOnlyList<Message>
{
    private readonly Message _message;

    internal SingleMessageReadOnlyList(Message message)
    {
        _message = message;
    }

    public int Count => 1;

    public Message this[int index]
    {
        get
        {
            if (index != 0)
                throw new ArgumentOutOfRangeException(nameof(index));
            return _message;
        }
    }

    public IEnumerator<Message> GetEnumerator()
    {
        yield return _message;
    }

    IEnumerator IEnumerable.GetEnumerator()
    {
        return GetEnumerator();
    }
}
