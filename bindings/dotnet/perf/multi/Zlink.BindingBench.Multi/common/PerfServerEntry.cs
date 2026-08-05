using System;

internal static partial class PerfRunner
{
    internal static int RunMultiServer(string pattern, string transport, int size)
    {
        string outputPattern = NormalizePerfPattern(pattern);
        size = Math.Max(1, size);

        try
        {
            var options = PerfOptions.CreateMulti(PerfExecutionKind.MultiServer,
                outputPattern, transport, size, string.Empty);
            if (!MultiPerfPatternRegistry.TryGet(outputPattern,
                    out IPerfPattern perfPattern))
            {
                return 1;
            }

            int rc = perfPattern.RunMultiServer(options);
            if (rc != 0)
                return rc;

            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"multi_server_error:{ex.GetType().Name}:{ex.Message}\n{ex}");
            return 2;
        }
    }

    internal static int PrintUnsupported(string pattern, string transport,
        int size, string reason)
    {
        return PerfShared.PrintUnsupported(pattern, transport, size, reason);
    }

    internal static int PrintExternalStreamClientError()
    {
        Console.Error.WriteLine(
            "multi_client_error:stream_client_managed_entry_disabled");
        return 1;
    }
}
