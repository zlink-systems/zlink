using System;
using System.Collections.Generic;
using Systems.Zlink.Stream.Connector.Contracts;

namespace Systems.Zlink.Stream.Connector.Runtime
{
    /// <summary>
    ///     Unread received messages, per packet name.
    /// </summary>
    /// <remarks>
    ///     Mirrors the native connector's history, including the rule that a wait predicate
    ///     skips a message instead of consuming it. The history lives on this side of the
    ///     boundary because the predicates are C# delegates: evaluating them in JavaScript
    ///     would mean calling into C# from a promise continuation, which this adapter never
    ///     does. WebGL is single-threaded, so no lock is needed.
    /// </remarks>
    internal sealed class ZlinkStreamReceivedMessages
    {
        private readonly Dictionary<string, List<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>> _messages =
            new Dictionary<string, List<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>>(StringComparer.Ordinal);

        public int Count(string name)
        {
            return _messages.TryGetValue(name, out var messages) ? messages.Count : 0;
        }

        public void Record(ZlinkStreamMessage<ZlinkStreamEncodedPayload> message)
        {
            if (!_messages.TryGetValue(message.Name, out var messages))
            {
                messages = new List<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>();
                _messages.Add(message.Name, messages);
            }

            messages.Add(message);
        }

        public ZlinkStreamMessage<ZlinkStreamEncodedPayload> TryTake(
            string name,
            Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate)
        {
            if (!_messages.TryGetValue(name, out var messages)) return null;

            for (var index = 0; index < messages.Count; index++)
            {
                var message = messages[index];
                if (predicate != null && !predicate(message)) continue;

                messages.RemoveAt(index);
                if (messages.Count == 0) _messages.Remove(name);
                return message;
            }

            return null;
        }
    }
}
