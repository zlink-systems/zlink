using System.Threading.Channels;
using DeliveryDispatch.Shared.Contracts;

namespace DeliveryDispatch.Server.Dispatch;

internal sealed class DispatchWorkQueue
{
    private readonly Channel<AssignDeliveryMsg> _queue = Channel.CreateUnbounded<AssignDeliveryMsg>();

    public ValueTask EnqueueAsync(
        AssignDeliveryMsg request,
        CancellationToken cancellationToken)
    {
        return _queue.Writer.WriteAsync(request, cancellationToken);
    }

    public IAsyncEnumerable<AssignDeliveryMsg> ReadAllAsync(CancellationToken cancellationToken)
    {
        return _queue.Reader.ReadAllAsync(cancellationToken);
    }
}
