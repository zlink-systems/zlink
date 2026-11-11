using System.IO.Compression;
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
        // XML documentation is shipped with every assembly but its text is not the API contract
        // (the signature snapshot is). Require a non-empty, parseable doc per assembly so a
        // missing or broken file still fails, without pinning the comment text.
        foreach (var assembly in archive.Entries
                     .Where(static entry => entry.FullName.StartsWith("lib/", StringComparison.Ordinal)
                                            && entry.FullName.EndsWith(".dll", StringComparison.Ordinal))
                     .OrderBy(static entry => entry.FullName, StringComparer.Ordinal))
        {
            var docPath = assembly.FullName[..^".dll".Length] + ".xml";
            var doc = archive.GetEntry(docPath)
                ?? throw new InvalidOperationException($"{assembly.FullName} has no XML documentation entry {docPath}.");
            if (doc.Length == 0)
                throw new InvalidOperationException($"XML documentation {docPath} is empty.");
            using (var content = doc.Open())
            {
                var root = XDocument.Load(content).Root;
                if (root?.Name.LocalName != "doc")
                    throw new InvalidOperationException($"XML documentation {docPath} has no doc root.");
            }
            lines.Add($"xml-doc {docPath}");
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
                lines.Add($"metadata repository{FormatAttributes(repository, version, buildContext: true)}");

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
        bool buildContext = false)
    {
        // The repository commit and branch come from the build checkout, not from the package
        // contract: a merge-queue build records its queue branch, a pull request build records none.
        var values = element.Attributes()
            .Where(attribute => !(buildContext && attribute.Name.LocalName == "branch"))
            .OrderBy(static attribute => attribute.Name.LocalName, StringComparer.Ordinal)
            .Select(attribute =>
            {
                var value = buildContext && attribute.Name.LocalName == "commit"
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
