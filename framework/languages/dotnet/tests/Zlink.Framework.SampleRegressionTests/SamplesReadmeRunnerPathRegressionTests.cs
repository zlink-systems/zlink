using System.Text.RegularExpressions;
using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    // e106104ffe deleted the per-language aggregate runners (run_samples.sh / run_samples.ps1)
    // but left three samples READMEs naming them, so a reader who followed C++, Java, or Node
    // was stopped by "No such file or directory" on the first command (#585). The cpp and java
    // contract suites already assert that the aggregate runners stay absent; nothing asserted
    // that the documents agree. This does: every runner path a samples README prints has to
    // resolve to a file, and every language's samples directory has to print at least one.
    [Fact]
    public void Samples_Readmes_Name_Runner_Scripts_That_Exist()
    {
        var repositoryRoot = Path.GetFullPath(Path.Combine(
            ResolveDotnetRoot(), "..", "..", ".."));
        var languagesRoot = Path.Combine(repositoryRoot, "framework", "languages");

        // A path is a token that carries a directory separator; a bare `run_sample.sh` in prose
        // names the file class, not a command, and resolves nowhere.
        var runnerPath = new Regex(
            @"(?<path>[A-Za-z0-9_.\-]+(?:[\\/][A-Za-z0-9_.\-]+)*[\\/]run_samples?\.(?:sh|ps1))",
            RegexOptions.CultureInvariant);

        var samplesDirectories = Directory
            .EnumerateDirectories(languagesRoot)
            .Select(language => Path.Combine(language, "samples"))
            .Where(Directory.Exists)
            .OrderBy(path => path, StringComparer.Ordinal)
            .ToArray();
        Assert.NotEmpty(samplesDirectories);

        var unresolved = new List<string>();
        var silent = new List<string>();

        foreach (var samplesDirectory in samplesDirectories)
        {
            var readmes = Directory
                .EnumerateFiles(samplesDirectory, "README*.md", SearchOption.TopDirectoryOnly)
                .OrderBy(path => path, StringComparer.Ordinal)
                .ToArray();
            var named = 0;

            foreach (var readme in readmes)
            {
                foreach (Match match in runnerPath.Matches(ReadSource(readme)))
                {
                    named++;
                    var quoted = match.Groups["path"].Value.Replace('\\', '/');
                    var relative = quoted.StartsWith("./", StringComparison.Ordinal)
                        ? quoted[2..]
                        : quoted;
                    var bases = new[]
                    {
                        repositoryRoot,
                        samplesDirectory,
                        Directory.GetParent(samplesDirectory)!.FullName
                    };
                    if (!bases.Any(root => File.Exists(Path.Combine(root, relative))))
                        unresolved.Add(
                            $"{NormalizeRelativePath(Path.GetRelativePath(repositoryRoot, readme))}: {quoted}");
                }
            }

            if (named == 0)
                silent.Add(NormalizeRelativePath(Path.GetRelativePath(repositoryRoot, samplesDirectory)));
        }

        Assert.Empty(unresolved);
        Assert.Empty(silent);
    }
}
