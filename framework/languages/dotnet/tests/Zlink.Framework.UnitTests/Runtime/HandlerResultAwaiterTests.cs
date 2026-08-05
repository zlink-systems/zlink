namespace Zlink.Framework.UnitTests;

public sealed class HandlerResultAwaiterTests
{
    [Fact]
    public async Task AwaitAsync_Returns_ValueTaskOfT_Result()
    {
        object result = new ValueTask<int>(42);

        var awaited = await ZLinkHandlerResultAwaiter.AwaitAsync(result);

        Assert.Equal(42, awaited);
    }
}