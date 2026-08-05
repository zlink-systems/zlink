using Systems.Zlink;

namespace Zlink.Framework.Runtime.Backend.Contracts
{
    internal static class ContractNamespaceMarker { }
}

namespace Zlink.Framework.Runtime.Service
{
    internal readonly record struct MeshOperationId(ulong High, ulong Low);

    internal readonly record struct MeshReceiveRecord(
        MeshOperationId OperationId,
        int TerminalResult,
        int FailureErrno)
    {
        internal static MeshReceiveRecord CompletionFailure(
            MeshOperationId operationId,
            RequestResult result) =>
            new(operationId, (int) result, 0);
    }
}

namespace Zlink.Framework.Runtime.Backend.DotNet
{
    internal static class ZLinkMessageParts
    {
        internal static void DisposeAll(IReadOnlyList<Message> parts)
        {
            foreach (var part in parts) part.Dispose();
        }
    }
}

namespace Zlink.Framework.Runtime.Diagnostics
{
    // M5 compiles the completion table as an isolated source slice. Keep the
    // slice independent of the full runtime metrics graph; the production
    // project supplies the real implementation.
    internal static class ZLinkRuntimeMetrics
    {
        internal static void RecordMessageDropped(
            string meshName,
            string surface,
            string kind,
            string reason)
        {
        }
    }
}
