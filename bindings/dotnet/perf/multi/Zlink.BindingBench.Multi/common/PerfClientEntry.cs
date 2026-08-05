using System;

internal static partial class PerfRunner
{
    internal static int RunMultiClient(string pattern, string transport, int size,
        string endpoint, string controlEndpoint = "")
    {
        string outputPattern = NormalizePerfPattern(pattern);
        size = Math.Max(1, size);

        if (!ParseEndpointArg(endpoint, out string normalizedEndpoint))
            return 1;

        try
        {
            var options = PerfOptions.CreateMulti(PerfExecutionKind.MultiClient,
                outputPattern, transport, size, normalizedEndpoint,
                controlEndpoint);
            if (!MultiPerfPatternRegistry.TryGet(outputPattern,
                    out IPerfPattern perfPattern))
            {
                return 1;
            }

            int rc = perfPattern.RunMultiClient(options);
            if (rc != 0)
                return rc;

            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"multi_client_error:{ex.GetType().Name}:{ex.Message}\n{ex}");
            return 2;
        }
    }
}
