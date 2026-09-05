using System;
using System.Collections.Generic;
using Systems.Zlink;

public static class PerfSocketIo
{
    // Each size runs in its own process with a fixed part-count setting.
    public static int MeasurementPartCount { get; } =
        string.Equals(Environment.GetEnvironmentVariable("PERF_PART_COUNT"), "1",
            StringComparison.Ordinal) ? 1 : 2;

    public static bool TryMeasurementPayload(IReadOnlyList<Message> parts,
        out Message payload)
    {
        payload = null!;
        if (parts == null || parts.Count != MeasurementPartCount)
            return false;
        if (MeasurementPartCount == 2 && parts[1].Size != 0)
            return false;
        payload = parts[0];
        return true;
    }

    private static Message MeasurementTail() => Message.Allocate(0);
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

    public static Task SendMeasurementAsync(IDealerSocket socket, byte[] payload,
        SendFlags flags = SendFlags.None)
    {
        _ = flags;
        Message message = CreatePooledMessage(payload);
        Message? tail = MeasurementPartCount == 2 ? MeasurementTail() : null;
        try
        {
            Task submit = tail != null
                ? socket.Send().Message(message).Message(tail).Async()
                : socket.Send().Message(message).Async();
            return CompleteMeasurementSend(submit, tail, message);
        }
        catch
        {
            tail?.Dispose();
            message.Dispose();
            throw;
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

    public static Task SendMeasurementAsync(IDealerSocket socket,
        Message message, SendFlags flags = SendFlags.None)
    {
        _ = flags;
        Message? tail = MeasurementPartCount == 2 ? MeasurementTail() : null;
        try
        {
            Task submit = tail != null
                ? socket.Send().Message(message).Message(tail).Async()
                : socket.Send().Message(message).Async();
            return CompleteMeasurementSend(submit, tail, ownedMessage: null);
        }
        catch
        {
            tail?.Dispose();
            throw;
        }
    }

    public static ValueTask<int> SendAsync(IRouterSocket socket,
        RoutingId routingId, byte[] payload, SendFlags flags = SendFlags.None)
    {
        _ = flags;
        Message message = CreatePooledMessage(payload);
        try
        {
            Task submit = socket.Send(routingId).Message(message).Async();
            if (submit.IsCompletedSuccessfully)
            {
                message.Dispose();
                return new ValueTask<int>(payload.Length);
            }
            return AwaitTargetedSendAsync(submit, message, payload.Length);
        }
        catch
        {
            message.Dispose();
            throw;
        }
    }

    public static Task SendMeasurementAsync(IRouterSocket socket,
        RoutingId routingId, Message message, SendFlags flags = SendFlags.None)
    {
        _ = flags;
        Message? tail = MeasurementPartCount == 2 ? MeasurementTail() : null;
        try
        {
            Task submit = tail != null
                ? socket.Send(routingId).Message(message).Message(tail).Async()
                : socket.Send(routingId).Message(message).Async();
            return CompleteMeasurementSend(submit, tail, ownedMessage: null);
        }
        catch
        {
            tail?.Dispose();
            throw;
        }
    }

    public static Task SendMeasurementAsync(IRouterSocket socket,
        RoutingId routingId, byte[] payload, SendFlags flags = SendFlags.None)
    {
        _ = flags;
        Message message = CreatePooledMessage(payload);
        Message? tail = MeasurementPartCount == 2 ? MeasurementTail() : null;
        try
        {
            Task submit = tail != null
                ? socket.Send(routingId).Message(message).Message(tail).Async()
                : socket.Send(routingId).Message(message).Async();
            return CompleteMeasurementSend(submit, tail, message);
        }
        catch
        {
            tail?.Dispose();
            message.Dispose();
            throw;
        }
    }

    private static Task CompleteMeasurementSend(Task submit, Message? tail,
        Message? ownedMessage)
    {
        if (!submit.IsCompletedSuccessfully)
            return AwaitMeasurementSendAsync(submit, tail, ownedMessage);

        tail?.Dispose();
        ownedMessage?.Dispose();
        return Task.CompletedTask;
    }

    private static async Task AwaitMeasurementSendAsync(Task submit,
        Message? tail, Message? ownedMessage)
    {
        try
        {
            await submit.ConfigureAwait(false);
        }
        finally
        {
            tail?.Dispose();
            ownedMessage?.Dispose();
        }
    }

    private static async ValueTask<int> AwaitTargetedSendAsync(Task submit,
        Message message, int size)
    {
        try
        {
            await submit.ConfigureAwait(false);
            return size;
        }
        finally
        {
            message.Dispose();
        }
    }

    public static ValueTask<int> SendAsync(IRouterSocket socket,
        RoutingId routingId,
        Message message, SendFlags flags = SendFlags.None)
    {
        _ = flags;
        int size = message.Size;
        Task submit = socket.Send(routingId).Message(message).Async();
        if (submit.IsCompletedSuccessfully)
            return new ValueTask<int>(size);
        return AwaitTargetedSendAsync(submit, size);
    }

    private static async ValueTask<int> AwaitTargetedSendAsync(Task submit,
        int size)
    {
        await submit.ConfigureAwait(false);
        return size;
    }

    public static int Send(IMessageSocket socket, ReadOnlySpan<byte> payload,
        SendFlags flags = SendFlags.None)
    {
        Message message = CreatePooledMessage(payload);
        try
        {
            var submit = socket.Send().Message(message);
            submit.Submit();
            return payload.Length;
        }
        catch (ZlinkSubmitException ex)
            when (ex.Result == ZlinkSubmitException.ErrorCode.Backpressured)
        {
            return 0;
        }
        finally
        {
            message.Dispose();
        }
    }

    public static int SendMeasurement(IMessageSocket socket, ReadOnlySpan<byte> payload,
        SendFlags flags = SendFlags.None)
    {
        Message message = CreatePooledMessage(payload);
        Message? tail = MeasurementPartCount == 2 ? MeasurementTail() : null;
        try
        {
            var submit = socket.Send().Message(message);
            if (tail != null)
                submit = submit.Message(tail);
            submit.Submit();
            return payload.Length;
        }
        catch (ZlinkSubmitException ex)
            when (ex.Result == ZlinkSubmitException.ErrorCode.Backpressured)
        {
            return 0;
        }
        finally
        {
            tail?.Dispose();
            message.Dispose();
        }
    }

    public static int Send(IMessageSocket socket, Message message,
        SendFlags flags = SendFlags.None)
    {
        int size = message.Size;
        try
        {
            socket.Send().Message(message).Submit();
            return size;
        }
        catch (ZlinkSubmitException ex)
            when (ex.Result == ZlinkSubmitException.ErrorCode.Backpressured)
        {
            return 0;
        }
    }

    public static int SendMeasurement(IDealerSocket socket,
        ReadOnlySpan<byte> payload, SendFlags flags = SendFlags.None)
    {
        Message message = CreatePooledMessage(payload);
        Message? tail = MeasurementPartCount == 2 ? MeasurementTail() : null;
        try
        {
            var submit = socket.Send().Message(message);
            if (tail != null)
                submit = submit.Message(tail);
            submit.Submit();
            return payload.Length;
        }
        catch (ZlinkSubmitException ex)
            when (ex.Result == ZlinkSubmitException.ErrorCode.Backpressured)
        {
            return 0;
        }
        finally
        {
            tail?.Dispose();
            message.Dispose();
        }
    }

    public static int Send(IRouterSocket socket, string routingId,
        ReadOnlySpan<byte> payload, SendFlags flags = SendFlags.None)
    {
        return Send(socket, RoutingId.From(System.Text.Encoding.UTF8.GetBytes(routingId)),
            payload, flags);
    }

    public static int Send(IRouterSocket socket, RoutingId routingId,
        ReadOnlySpan<byte> payload, SendFlags flags = SendFlags.None)
    {
        Message message = CreatePooledMessage(payload);
        try
        {
            var submit = socket.Send(routingId).Message(message);
            submit.Submit();
            return payload.Length;
        }
        catch (ZlinkSubmitException ex)
            when (ex.Result == ZlinkSubmitException.ErrorCode.Backpressured)
        {
            return 0;
        }
        finally
        {
            message.Dispose();
        }
    }

    public static int SendMeasurement(IRouterSocket socket, RoutingId routingId,
        ReadOnlySpan<byte> payload, SendFlags flags = SendFlags.None)
    {
        Message message = CreatePooledMessage(payload);
        Message? tail = MeasurementPartCount == 2 ? MeasurementTail() : null;
        try
        {
            var submit = socket.Send(routingId).Message(message);
            if (tail != null)
                submit = submit.Message(tail);
            submit.Submit();
            return payload.Length;
        }
        catch (ZlinkSubmitException ex)
            when (ex.Result == ZlinkSubmitException.ErrorCode.Backpressured)
        {
            return 0;
        }
        finally
        {
            tail?.Dispose();
            message.Dispose();
        }
    }

    public static int Send(IRouterSocket socket, RoutingId routingId,
        Message message, SendFlags flags = SendFlags.None)
    {
        int size = message.Size;
        try
        {
            socket.Send(routingId).Message(message).Submit();
            return size;
        }
        catch (ZlinkSubmitException ex)
            when (ex.Result == ZlinkSubmitException.ErrorCode.Backpressured)
        {
            return 0;
        }
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

    public static int PublishMeasurement(IPublisherSocket socket, string topic,
        ReadOnlySpan<byte> payload, SendFlags flags = SendFlags.None)
    {
        Message message = CreatePooledMessage(payload);
        Message? tail = MeasurementPartCount == 2 ? MeasurementTail() : null;
        try
        {
            var submit = socket.TryPublish(topic).Message(message);
            if (tail != null)
                submit = submit.Message(tail);
            bool sent = flags == SendFlags.None
                ? submit.Submit()
                : submit.Flags(flags).Submit();
            return sent ? payload.Length : 0;
        }
        finally
        {
            tail?.Dispose();
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
