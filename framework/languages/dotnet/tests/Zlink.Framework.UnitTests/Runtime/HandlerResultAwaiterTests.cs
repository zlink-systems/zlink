namespace Zlink.Framework.UnitTests;

public sealed class HandlerResultAwaiterTests
{
    [Fact]
    public async Task InvocationPreservesCancelableTokensAndReusesTheUncancelableArgument()
    {
        object? Invoke(CancellationToken token) =>
            ZLinkHandlerInvocationEngine
                .InvokeAsync(
                    this,
                    static (_, argument, _, _, _, _) => argument,
                    new[] { ZLinkHandlerArgumentKind.CancellationToken },
                    null,
                    null!,
                    token
                )
                .GetAwaiter()
                .GetResult();

        var first = Invoke(CancellationToken.None);
        var second = Invoke(default);
        Assert.Same(first, second);
        Assert.Equal(CancellationToken.None, Assert.IsType<CancellationToken>(first));
        using var cancellation = new CancellationTokenSource();
        var captured = Assert.IsType<CancellationToken>(Invoke(cancellation.Token));
        Assert.Equal(cancellation.Token, captured);
        await cancellation.CancelAsync();
        Assert.True(captured.IsCancellationRequested);
        Assert.True(
            Assert.IsType<CancellationToken>(Invoke(cancellation.Token)).IsCancellationRequested
        );
    }

    [Fact]
    public async Task AwaitAsync_Returns_ValueTaskOfT_Result()
    {
        object result = new ValueTask<int>(42);

        var awaited = await ZLinkHandlerResultAwaiter.AwaitAsync(result);

        Assert.Equal(42, awaited);
    }
}
