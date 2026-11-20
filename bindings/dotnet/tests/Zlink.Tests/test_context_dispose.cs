using System.Threading;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_context_dispose
{
    // Core allows exactly one zlink_ctx_term per context (core/doc/spec/core/01-context.en.md
    // zlink_ctx_term). Concurrent Dispose callers must therefore share one native term.
    [Fact]
    public void concurrent_dispose_terms_native_context_once()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        const int threadCount = 16;
        for (var iteration = 0; iteration < 200; iteration++)
        {
            var context = (Context)Zlink.CreateContext();
            var termCalls = 0;
            // Only the first call reaches Core, so a regression is a count
            // failure instead of a crash on a freed context.
            context.TermNative = handle =>
            {
                if (Interlocked.Increment(ref termCalls) != 1)
                    return 0;
                Thread.Sleep(2);
                return NativeMethodsForTest.Term(handle);
            };

            using var start = new Barrier(threadCount);
            var errors = new Exception?[threadCount];
            var threads = new Thread[threadCount];
            for (var i = 0; i < threadCount; i++)
            {
                var slot = i;
                threads[i] = new Thread(() =>
                {
                    start.SignalAndWait();
                    try
                    {
                        context.Dispose();
                    }
                    catch (Exception ex)
                    {
                        errors[slot] = ex;
                    }
                });
                threads[i].Start();
            }
            foreach (var thread in threads)
                thread.Join();

            Assert.All(errors, Assert.Null);
            Assert.Equal(1, Volatile.Read(ref termCalls));
            Assert.Throws<ObjectDisposedException>(() => context.Options.IoThreads);
            context.Dispose();
            Assert.Equal(1, Volatile.Read(ref termCalls));
        }
    }

    [Fact]
    public void failed_term_keeps_context_open_and_retry_terms_again()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        var context = (Context)Zlink.CreateContext();
        var termCalls = 0;
        context.TermNative = handle =>
        {
            if (Interlocked.Increment(ref termCalls) == 1)
                return (int)CloseResult.Busy;
            return NativeMethodsForTest.Term(handle);
        };

        Assert.Throws<ZlinkCloseException>(() => context.Dispose());
        Assert.Equal(1, termCalls);
        _ = context.Options.IoThreads;

        context.Dispose();
        Assert.Equal(2, termCalls);
        context.Dispose();
        Assert.Equal(2, termCalls);
    }
}

internal static class NativeMethodsForTest
{
    internal static int Term(IntPtr handle) =>
        Systems.Zlink.Runtime.Native.NativeMethods.zlink_ctx_term(handle);
}
