using System.Diagnostics.Metrics;
using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.UnitTests;

public sealed class ZLinkObservationQueueTests
{
    [Fact]
    public async Task Intermediate_updates_are_coalesced_and_loss_is_per_observer()
    {
        var first = new ZLinkObservationQueue<TestStatus>(
            static status => status.Source,
            terminalCapacity: 2
        );
        var second = new ZLinkObservationQueue<TestStatus>(
            static status => status.Source,
            terminalCapacity: 2
        );
        await using var firstReader = first.ReadAllAsync().GetAsyncEnumerator();
        await using var secondReader = second.ReadAllAsync().GetAsyncEnumerator();

        first.Publish(new TestStatus("A", 1), terminal: false);
        first.Publish(new TestStatus("A", 2), terminal: false);
        first.Publish(new TestStatus("A", 3), terminal: false);
        second.Publish(new TestStatus("B", 10), terminal: false);

        Assert.True(await firstReader.MoveNextAsync());
        Assert.Equal(3UL, firstReader.Current.Status.Sequence);
        Assert.Equal(2UL, firstReader.Current.Loss.CoalescedCount);
        Assert.Equal(0UL, firstReader.Current.Loss.DiscardedTerminalCount);

        Assert.True(await secondReader.MoveNextAsync());
        Assert.Equal(10UL, secondReader.Current.Status.Sequence);
        Assert.Equal(0UL, secondReader.Current.Loss.CoalescedCount);

        first.Complete();
        second.Complete();
        Assert.False(await firstReader.MoveNextAsync());
        Assert.False(await secondReader.MoveNextAsync());
    }

    [Fact]
    public async Task Terminal_updates_are_preserved_and_oldest_terminal_is_counted_when_full()
    {
        var queue = new ZLinkObservationQueue<TestStatus>(
            static status => status.Source,
            terminalCapacity: 2
        );
        await using var reader = queue.ReadAllAsync().GetAsyncEnumerator();

        queue.Publish(new TestStatus("A", 1), terminal: false);
        queue.Publish(new TestStatus("A", 2), terminal: true);
        queue.Publish(new TestStatus("A", 3), terminal: false);
        queue.Publish(new TestStatus("A", 4), terminal: true);
        queue.Publish(new TestStatus("A", 5), terminal: true);
        queue.Complete();

        Assert.True(await reader.MoveNextAsync());
        Assert.Equal(4UL, reader.Current.Status.Sequence);
        Assert.Equal(2UL, reader.Current.Loss.CoalescedCount);
        Assert.Equal(1UL, reader.Current.Loss.DiscardedTerminalCount);
        Assert.True(await reader.MoveNextAsync());
        Assert.Equal(5UL, reader.Current.Status.Sequence);
        Assert.Equal(reader.Current.Loss, new ZLinkObservationLoss(2, 1));
        Assert.False(await reader.MoveNextAsync());
    }

    [Fact]
    public void Terminal_overflow_records_the_observer_event_source()
    {
        var samples = new List<string?>();
        using var listener = new MeterListener
        {
            InstrumentPublished = (instrument, owner) =>
            {
                if (
                    instrument.Meter.Name == ZLinkMeters.Framework
                    && instrument.Name == "zlink.observability.events.overflow"
                )
                    owner.EnableMeasurementEvents(instrument);
            },
        };
        listener.SetMeasurementEventCallback<long>(
            (instrument, _, tags, _) =>
            {
                if (instrument.Name != "zlink.observability.events.overflow")
                    return;
                samples.Add(Tag(tags, "source"));
            }
        );
        listener.Start();

        var queue = new ZLinkObservationQueue<TestStatus>(
            static status => status.Source,
            terminalCapacity: 1,
            eventName: "unit-test"
        );
        queue.Publish(new TestStatus("A", 1), terminal: true);
        queue.Publish(new TestStatus("B", 2), terminal: true);

        Assert.Equal(["unit-test"], samples);
    }

    [Fact]
    public async Task Initial_status_is_coalesced_by_the_existing_source_slot()
    {
        var queue = new ZLinkObservationQueue<TestStatus>(
            new TestStatus("channel", 1),
            false,
            static status => status.Source,
            "unit-test"
        );
        queue.Publish(new TestStatus("channel", 2), terminal: false);
        queue.Complete();
        await using var reader = queue.ReadAllAsync().GetAsyncEnumerator();
        Assert.True(await reader.MoveNextAsync());
        Assert.Equal(2UL, reader.Current.Status.Sequence);
        Assert.Equal(new ZLinkObservationLoss(1, 0), reader.Current.Loss);
        Assert.False(await reader.MoveNextAsync());
    }

    [Fact]
    public async Task Terminal_source_slot_blocks_intermediate_until_delivery()
    {
        var queue = new ZLinkObservationQueue<TestStatus>(
            static status => status.Source,
            terminalCapacity: 2
        );
        queue.Publish(new TestStatus("A", 1), terminal: true);
        queue.Publish(new TestStatus("A", 2), terminal: false);
        await using var reader = queue.ReadAllAsync().GetAsyncEnumerator();

        Assert.True(await reader.MoveNextAsync());
        Assert.Equal(1UL, reader.Current.Status.Sequence);
        Assert.Equal(1UL, reader.Current.Loss.CoalescedCount);

        queue.Publish(new TestStatus("A", 3), terminal: false);
        Assert.True(await reader.MoveNextAsync());
        Assert.Equal(3UL, reader.Current.Status.Sequence);
        queue.Complete();
        Assert.False(await reader.MoveNextAsync());
    }

    [Fact]
    public async Task Terminal_source_slot_is_released_after_oldest_discard()
    {
        var queue = new ZLinkObservationQueue<TestStatus>(
            static status => status.Source,
            terminalCapacity: 1
        );
        queue.Publish(new TestStatus("A", 1), terminal: true);
        queue.Publish(new TestStatus("A", 2), terminal: false);
        queue.Publish(new TestStatus("B", 3), terminal: true);
        queue.Publish(new TestStatus("A", 4), terminal: false);
        queue.Complete();

        var retained = new List<ZLinkObservedStatus<TestStatus>>();
        await foreach (var status in queue.ReadAllAsync())
            retained.Add(status);
        Assert.Contains(retained, item => item.Status == new TestStatus("B", 3));
        Assert.Contains(retained, item => item.Status == new TestStatus("A", 4));
        Assert.DoesNotContain(retained, item => item.Status.Sequence == 2);
        Assert.All(
            retained,
            item =>
            {
                Assert.Equal(1UL, item.Loss.CoalescedCount);
                Assert.Equal(1UL, item.Loss.DiscardedTerminalCount);
            }
        );
    }

    private static string? Tag(ReadOnlySpan<KeyValuePair<string, object?>> tags, string name)
    {
        foreach (var tag in tags)
            if (tag.Key == name)
                return tag.Value as string;
        return null;
    }

    private sealed record TestStatus(string Source, ulong Sequence);
}
