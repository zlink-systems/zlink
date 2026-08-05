namespace Zlink.Framework.Runtime.Messaging;

internal sealed class ZLinkOneWayCallGate(string operationName)
{
    private int _submitted;

    public void Claim()
    {
        if (Interlocked.Exchange(ref _submitted, 1) == 0) return;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"{operationName} was already submitted.");
    }
}
