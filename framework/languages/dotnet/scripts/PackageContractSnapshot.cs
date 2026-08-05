using System.IO.Compression;
using System.Security.Cryptography;
using System.Xml.Linq;

internal static class PackageContractSnapshot
{
    public static string Render(string packagePath, string version)
    {
        using var archive = ZipFile.OpenRead(packagePath);
        var nuspecEntry = archive.Entries.Single(
            static entry => entry.FullName.EndsWith(".nuspec", StringComparison.Ordinal));
        var lines = new List<string>();
        using (var stream = nuspecEntry.Open())
        {
            var document = XDocument.Load(stream);
            var metadata = document.Root?.Elements().Single(
                static element => element.Name.LocalName == "metadata")
                ?? throw new InvalidOperationException("The package nuspec has no metadata element.");
            var id = metadata.Elements().Single(
                static element => element.Name.LocalName == "id").Value;
            lines.Add($"package {id}");
        }

        foreach (var entry in archive.Entries
                     .Select(static entry => NormalizeEntry(entry.FullName))
                     .Order(StringComparer.Ordinal))
            lines.Add($"entry {entry}");
        foreach (var entry in archive.Entries
                     .Where(static entry => entry.FullName.StartsWith("lib/", StringComparison.Ordinal)
                                            && entry.FullName.EndsWith(".xml", StringComparison.Ordinal))
                     .OrderBy(static entry => entry.FullName, StringComparer.Ordinal))
        {
            using var content = entry.Open();
            lines.Add($"content-sha256 {entry.FullName}={Convert.ToHexString(SHA256.HashData(content)).ToLowerInvariant()}");
        }

        using (var stream = nuspecEntry.Open())
        {
            var document = XDocument.Load(stream);
            var metadata = document.Root?.Elements().Single(
                static element => element.Name.LocalName == "metadata")
                ?? throw new InvalidOperationException("The package nuspec has no metadata element.");

            foreach (var element in metadata.Elements()
                         .Where(static element => element.Name.LocalName is not "dependencies" and not "repository")
                         .OrderBy(static element => element.Name.LocalName, StringComparer.Ordinal))
                lines.Add($"metadata {element.Name.LocalName}={Normalize(element.Value, version)}{FormatAttributes(element, version)}");

            foreach (var repository in metadata.Elements()
                         .Where(static element => element.Name.LocalName == "repository"))
                lines.Add($"metadata repository{FormatAttributes(repository, version, normalizeCommit: true)}");

            foreach (var group in metadata.Descendants()
                         .Where(static element => element.Name.LocalName == "group")
                         .OrderBy(static element => Attribute(element, "targetFramework"), StringComparer.Ordinal))
            {
                var target = Attribute(group, "targetFramework");
                lines.Add($"dependency-group targetFramework={target}");
                foreach (var dependency in group.Elements()
                             .Where(static element => element.Name.LocalName == "dependency")
                             .OrderBy(static element => Attribute(element, "id"), StringComparer.Ordinal))
                    lines.Add(
                        $"dependency targetFramework={target}{FormatAttributes(dependency, version)}");
            }
        }

        return string.Join('\n', lines) + "\n";
    }

    private static string NormalizeEntry(string path)
    {
        const string prefix = "package/services/metadata/core-properties/";
        return path.StartsWith(prefix, StringComparison.Ordinal)
               && path.EndsWith(".psmdcp", StringComparison.Ordinal)
            ? $"{prefix}{{CORE_PROPERTIES}}.psmdcp"
            : path;
    }

    private static string FormatAttributes(
        XElement element,
        string version,
        bool normalizeCommit = false)
    {
        var values = element.Attributes()
            .OrderBy(static attribute => attribute.Name.LocalName, StringComparer.Ordinal)
            .Select(attribute =>
            {
                var value = normalizeCommit && attribute.Name.LocalName == "commit"
                    ? "{COMMIT}"
                    : Normalize(attribute.Value, version);
                return $" {attribute.Name.LocalName}={value}";
            });
        return string.Concat(values);
    }

    private static string Normalize(string value, string version) =>
        value.Replace(version, "{VERSION}", StringComparison.Ordinal).Trim();

    private static string Attribute(XElement element, string name) =>
        element.Attributes().FirstOrDefault(attribute => attribute.Name.LocalName == name)?.Value
        ?? string.Empty;
}
