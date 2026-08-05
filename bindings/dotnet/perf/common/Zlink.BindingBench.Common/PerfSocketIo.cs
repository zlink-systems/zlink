using System;
using Systems.Zlink;

public static class PerfSocketIo
{
    public static int Send(IMessageSocket socket, ReadOnlySpan<byte> payload,
        SendFlags flags = SendFlags.None)
    {
        Message message = CreatePooledMessage(payload);
        try
        {
            if (socket.Send().Message(message).Flags(flags).Submit())
                return payload.Length;
            return 0;
        }
        finally
        {
            message.Dispose();
        }
    }

    public static int Send(IMessageSocket socket, Message message,
        SendFlags flags = SendFlags.None)
    {
        int size = message.Size;
        return socket.Send().Message(message).Flags(flags).Submit()
            ? size
            : 0;
    }

    public static int Send(IRoutedMessageSocket socket, string routingId,
        ReadOnlySpan<byte> payload, SendFlags flags = SendFlags.None)
    {
        return Send(socket, RoutingId.From(System.Text.Encoding.UTF8.GetBytes(routingId)),
            payload, flags);
    }

    public static int Send(IRoutedMessageSocket socket, RoutingId routingId,
        ReadOnlySpan<byte> payload, SendFlags flags = SendFlags.None)
    {
        Message message = CreatePooledMessage(payload);
        try
        {
            if (socket.Send(routingId).Message(message).Flags(flags).Submit())
                return payload.Length;
            return 0;
        }
        finally
        {
            message.Dispose();
        }
    }

    public static int Send(IRoutedMessageSocket socket, RoutingId routingId,
        Message message, SendFlags flags = SendFlags.None)
    {
        int size = message.Size;
        return socket.Send(routingId).Message(message).Flags(flags).Submit()
            ? size
            : 0;
    }

    public static int Publish(IPublisherSocket socket, string topic,
        ReadOnlySpan<byte> payload, SendFlags flags = SendFlags.None)
    {
        Message message = CreatePooledMessage(payload);
        try
        {
            if (socket.Publish(topic).Message(message).Flags(flags).Submit())
                return payload.Length;
            return 0;
        }
        finally
        {
            message.Dispose();
        }
    }

    private static Message CreatePooledMessage(ReadOnlySpan<byte> payload)
    {
        // HOT PATH: a successful submit consumes the payload, but the caller
        // must still dispose the wrapper. The pool-backed public path prevents
        // one managed wrapper per message from being left for GC.
        Message message = Message.Allocate(payload.Length);
        payload.CopyTo(message.AsSpan());
        return message;
    }
}
