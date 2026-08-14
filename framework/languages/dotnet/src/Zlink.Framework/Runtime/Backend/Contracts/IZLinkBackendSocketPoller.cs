namespace Zlink.Framework.Runtime.Backend.Contracts;

[Flags]
internal enum ZLinkBackendSocketReadiness
{
    None = 0,
    Readable = 1,
    Writable = 2,
    Error = 4,
    Priority = 8
}

// Framework receive loops use this seam to wait for native socket readiness.
// The .NET backend implementation owns the public Systems.Zlink poller and the
// socket registration; callers only receive the readiness flags they need.
internal interface IZLinkBackendSocketPoller : IDisposable
{
    ZLinkBackendSocketReadiness Wait(TimeSpan timeout);
}
