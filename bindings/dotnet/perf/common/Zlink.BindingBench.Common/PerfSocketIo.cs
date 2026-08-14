using System;
using Systems.Zlink;

public static class PerfSocketIo
{
    public static async Task<int> SendAsync(IDealerSocket socket, byte[] payload,
        SendFlags flags = SendFlags.None)
    {
        _ = flags;
        Message message = CreatePooledMessage(payload);
        try
        {
            await socket.Send().Message(message).Async().ConfigureAwait(false);
            return payload.Length;
        }
        finally
        {
            message.Dispose();
        }
    }

    public static async Task<int> SendAsync(IDealerSocket socket, Message message,
        SendFlags flags = SendFlags.None)
    {
        _ = flags;
        int size = message.Size;
        await socket.Send().Message(message).Async().ConfigureAwait(false);
        return size;
    }

    public static async Task<int> SendAsync(IRouterSocket socket,
        RoutingId routingId, byte[] payload, SendFlags flags = SendFlags.None)
    {
        _ = flags;
        Message message = CreatePooledMessage(payload);
        try
        {
            await socket.Send(routingId).Message(message).Async()
                .ConfigureAwait(false);
            return payload.Length;
        }
        finally
        {
            message.Dispose();
        }
    }

    public static async Task<int> SendAsync(IRouterSocket socket,
        RoutingId routingId,
        Message message, SendFlags flags = SendFlags.None)
    {
        _ = flags;
        int size = message.Size;
        await socket.Send(routingId).Message(message).Async()
            .ConfigureAwait(false);
        return size;
    }

    public static int Send(IMessageSocket socket, ReadOnlySpan<byte> payload,
        SendFlags flags = SendFlags.None)
    {
        Message message = CreatePooledMessage(payload);
        try
        {
            var submit = socket.Send().Message(message);
            bool sent = flags == SendFlags.None
                ? submit.Submit()
                : submit.Flags(flags).Submit();
            if (sent)
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
            var submit = socket.Send(routingId).Message(message);
            bool sent = flags == SendFlags.None
                ? submit.Submit()
                : submit.Flags(flags).Submit();
            if (sent)
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
            var submit = socket.TryPublish(topic).Message(message);
            bool sent = flags == SendFlags.None
                ? submit.Submit()
                : submit.Flags(flags).Submit();
            if (sent)
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
        // must still dispose the wrapper. Managed bindings may use the
        // existing per-thread Message pool to avoid wrapper GC churn.
        Message message = Message.Allocate(payload.Length);
        payload.CopyTo(message.AsSpan());
        return message;
    }

}
