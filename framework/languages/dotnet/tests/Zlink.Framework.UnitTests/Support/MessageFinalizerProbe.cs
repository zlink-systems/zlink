using System.Diagnostics.Tracing;
using System.Runtime.CompilerServices;

namespace Zlink.Framework.UnitTests;

internal sealed class MessageFinalizerProbe : EventListener
{
    private long _finalizers;

    protected override void OnEventSourceCreated(EventSource source)
    {
        if (source.Name == "Microsoft-Windows-DotNETRuntime")
            EnableEvents(source, EventLevel.Verbose, (EventKeywords)1);
    }

    protected override void OnEventWritten(EventWrittenEventArgs data)
    {
        if (
            data.EventName == "FinalizeObject"
            && ulong.TryParse(data.Payload![0]?.ToString(), out var typeId)
            && typeId == unchecked((ulong)typeof(Message).TypeHandle.Value.ToInt64())
        )
            Interlocked.Increment(ref _finalizers);
    }

    internal async Task<long> CountAsync(Func<Task> operation)
    {
        await CollectAsync();
        Interlocked.Exchange(ref _finalizers, 0);
        await operation();
        await CollectAsync();
        return Interlocked.Read(ref _finalizers);
    }

    private static async Task CollectAsync()
    {
        for (var pass = 0; pass < 2; pass++)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            // Allow setup frames and exiting-thread TLS pools to become
            // unreachable before the next collection and measurement reset.
            // Runtime finalizer events are also delivered asynchronously.
            await Task.Delay(250);
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    internal static void CreateMessages(bool dispose)
    {
        for (var index = 0; index < 16; index++)
        {
            var message = Message.From("finalizer-detector-control");
            if (dispose)
                message.Dispose();
        }
    }
}
