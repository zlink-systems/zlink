/* SPDX-License-Identifier: Apache-2.0 */

namespace Zlink.HttpClient.Runtime;

internal static class HttpHeaderLookup
{
    public static string? Find(IReadOnlyDictionary<string, string> headers, string name)
    {
        return headers.TryGetValue(name, out var value) ? value : null;
    }

    public static IReadOnlyDictionary<string, string> Without(
        IReadOnlyDictionary<string, string> headers,
        params string[] names)
    {
        var copy = new Dictionary<string, string>(headers, StringComparer.OrdinalIgnoreCase);
        foreach (var name in names) copy.Remove(name);
        return copy;
    }
}
