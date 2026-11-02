namespace Zlink.Framework.UnitTests;

// The allocation tests compare PerCallAllocation results strictly, so the helper itself
// must report a per-call cost and must not report one where there is none.
public sealed class PerCallAllocationTests
{
    private static object? _sink;

    [Fact]
    public void Reports_zero_for_a_call_that_does_not_allocate()
    {
        var value = 0;
        Assert.Equal(0, PerCallAllocation.MinWindow(() => value++, callsPerWindow: 64, warmup: 8));
    }

    [Fact]
    public void Reports_a_per_call_allocation_in_every_window()
    {
        var bytes = PerCallAllocation.MinWindow(
            () => _sink = new byte[64],
            callsPerWindow: 64,
            warmup: 8
        );
        Assert.True(bytes >= 64 * 64, $"measured {bytes} bytes for 64 allocations of 64 bytes");
    }

    [Theory]
    [InlineData("single")]
    [InlineData("pair")]
    [InlineData("async")]
    public async Task Skips_windows_interrupted_by_gc_on_another_thread(string mode)
    {
        using var requested = new SemaphoreSlim(0);
        using var collected = new SemaphoreSlim(0);
        var stopping = 0;
        var collector = new Thread(() =>
        {
            while (true)
            {
                requested.Wait();
                if (Volatile.Read(ref stopping) != 0)
                    return;
                GC.Collect();
                collected.Release();
            }
        })
        {
            IsBackground = true,
        };
        collector.Start();
        var collectionsToForce = mode == "pair" ? 8 : 4;
        var collectionIndex = 0;
        void Allocate()
        {
            if (Interlocked.Increment(ref collectionIndex) <= collectionsToForce)
            {
                requested.Release();
                collected.Wait();
            }
            _sink = new byte[64];
        }

        try
        {
            var before = GC.GetAllocatedBytesForCurrentThread();
            _sink = new byte[64];
            var expected = GC.GetAllocatedBytesForCurrentThread() - before;
            switch (mode)
            {
                case "single":
                    Assert.Equal(
                        expected,
                        PerCallAllocation.MinWindow(_ => Allocate(), 1, windows: 2)
                    );
                    break;
                case "pair":
                    var pair = PerCallAllocation.MinWindowPair(
                        _ => Allocate(),
                        _ => Allocate(),
                        1,
                        windows: 2
                    );
                    Assert.Equal(expected, pair.First);
                    Assert.Equal(expected, pair.Second);
                    break;
                case "async":
                    Assert.Equal(
                        expected,
                        await PerCallAllocation.MinWindowAsync(
                            () =>
                            {
                                Allocate();
                                return ValueTask.CompletedTask;
                            },
                            1,
                            windows: 2
                        )
                    );
                    break;
            }
        }
        finally
        {
            Volatile.Write(ref stopping, 1);
            requested.Release();
            collector.Join();
        }
    }

    [Fact]
    public void Excludes_a_one_time_allocation_from_the_minimum()
    {
        var first = true;
        var bytes = PerCallAllocation.MinWindow(
            _ =>
            {
                if (first)
                    _sink = new byte[4096];
                first = false;
            },
            callsPerWindow: 64
        );
        Assert.Equal(0, bytes);
    }

    [Theory]
    [InlineData("single", 0)]
    [InlineData("single", 16 * 1024)]
    [InlineData("pair", 0)]
    [InlineData("pair", 16 * 1024)]
    [InlineData("async", 0)]
    [InlineData("async", 16 * 1024)]
    public async Task Measures_every_call_without_gc_accounting_noise(string mode, int payloadSize)
    {
        const int calls = 128;
        var before = GC.GetAllocatedBytesForCurrentThread();
        if (payloadSize != 0)
            _sink = new byte[payloadSize];
        var expected = (GC.GetAllocatedBytesForCurrentThread() - before) * calls;

        void Allocate()
        {
            if (payloadSize != 0)
                _sink = new byte[payloadSize];
        }

        switch (mode)
        {
            case "single":
                Assert.Equal(expected, PerCallAllocation.MinWindow(Allocate, calls));
                break;
            case "pair":
                var pair = PerCallAllocation.MinWindowPair(_ => Allocate(), _ => Allocate(), calls);
                Assert.Equal(expected, pair.First);
                Assert.Equal(expected, pair.Second);
                break;
            case "async":
                Assert.Equal(
                    expected,
                    await PerCallAllocation.MinWindowAsync(
                        () =>
                        {
                            Allocate();
                            return ValueTask.CompletedTask;
                        },
                        calls
                    )
                );
                break;
        }
    }

    [Theory]
    [InlineData("single")]
    [InlineData("pair")]
    [InlineData("async")]
    public async Task Rejects_a_collection_inside_the_measurement(string mode)
    {
        switch (mode)
        {
            case "single":
                Assert.Throws<InvalidOperationException>(() =>
                    PerCallAllocation.MinWindow(() => GC.Collect(), 1)
                );
                break;
            case "pair":
                Assert.Throws<InvalidOperationException>(() =>
                    PerCallAllocation.MinWindowPair(_ => { }, _ => GC.Collect(), 1)
                );
                break;
            case "async":
                await Assert.ThrowsAsync<InvalidOperationException>(() =>
                    PerCallAllocation.MinWindowAsync(
                        () =>
                        {
                            GC.Collect();
                            return ValueTask.CompletedTask;
                        },
                        1
                    )
                );
                break;
        }
        Assert.Equal(0, PerCallAllocation.MinWindow(() => { }, 1));
    }

    [Theory]
    [InlineData("single", 0)]
    [InlineData("single", 64)]
    [InlineData("pair-first", 0)]
    [InlineData("pair-first", 64)]
    [InlineData("pair-second", 0)]
    [InlineData("pair-second", 64)]
    [InlineData("async", 0)]
    [InlineData("async", 64)]
    public async Task Remeasures_an_interrupted_window_without_losing_per_call_cost(
        string mode,
        int payloadSize
    )
    {
        var before = GC.GetAllocatedBytesForCurrentThread();
        if (payloadSize != 0)
            _sink = new byte[payloadSize];
        var expected = GC.GetAllocatedBytesForCurrentThread() - before;
        var interrupted = false;
        void Allocate(bool interrupt)
        {
            if (interrupt && !interrupted)
            {
                interrupted = true;
                GC.Collect();
            }
            if (payloadSize != 0)
                _sink = new byte[payloadSize];
        }
        switch (mode)
        {
            case "single":
                Assert.Equal(
                    expected,
                    PerCallAllocation.MinWindow(() => Allocate(true), 1, windows: 1)
                );
                break;
            case "pair-first":
            case "pair-second":
                var pair = PerCallAllocation.MinWindowPair(
                    _ => Allocate(mode == "pair-first"),
                    _ => Allocate(mode == "pair-second"),
                    1,
                    windows: 1
                );
                Assert.Equal(expected, pair.First);
                Assert.Equal(expected, pair.Second);
                break;
            case "async":
                Assert.Equal(
                    expected,
                    await PerCallAllocation.MinWindowAsync(
                        () =>
                        {
                            Allocate(true);
                            return ValueTask.CompletedTask;
                        },
                        1,
                        windows: 1
                    )
                );
                break;
        }
        Assert.True(interrupted);
    }

    [Fact]
    public void Recreates_and_releases_window_state_for_each_attempt()
    {
        var started = 0;
        var released = 0;
        Assert.Equal(
            0,
            PerCallAllocation.MinWindow(
                _ =>
                {
                    if (started == 1)
                        GC.Collect();
                },
                1,
                windows: 1,
                beforeWindow: _ => started++,
                afterWindow: _ => released++
            )
        );
        Assert.Equal(2, started);
        Assert.Equal(started, released);
    }

    [Fact]
    public async Task Marks_a_window_with_a_collection_invalid()
    {
        var calls = 0;
        var measurement = await PerCallAllocation.MeasureWindowAsync<int>(() =>
        {
            calls++;
            GC.Collect();
            return new ValueTask<int>(0);
        });
        Assert.False(measurement.IsValid);
        Assert.Equal(0, measurement.Value);
        Assert.Equal(4, calls);
        Assert.Equal(0, PerCallAllocation.MinWindow(() => { }, 1));
    }

    [Fact]
    public void Preserves_an_action_failure_after_a_collection()
    {
        var failure = new InvalidOperationException("action failed after GC");
        Assert.Same(
            failure,
            Assert.Throws<InvalidOperationException>(() =>
                PerCallAllocation.MinWindow(
                    () =>
                    {
                        GC.Collect();
                        throw failure;
                    },
                    1
                )
            )
        );
    }

    [Fact]
    public void Allows_a_new_measurement_after_an_action_throws()
    {
        var failure = new InvalidOperationException("action failed");
        Assert.Same(
            failure,
            Assert.Throws<InvalidOperationException>(() =>
                PerCallAllocation.MinWindow(() => throw failure, 1)
            )
        );
        Assert.Equal(0, PerCallAllocation.MinWindow(() => { }, 1));
    }
}
