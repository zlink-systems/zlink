using System.Text.RegularExpressions;
using System.Xml.Linq;

namespace Zlink.Framework.UnitTests.Documentation;

public sealed class RegressionTests
{
    private static readonly string[] DotNetContractDocuments =
    [
        "README.ko.md",
        // 언어별 공개 계약은 시스템 구조, 인터페이스와 선택 capability의 정확한 시그니처를 고정한다.
        // 기능별 의미는 공통 스펙(framework/doc/framework/common/spec)이 소유한다.
        "01-system-structure.ko.md",
        "02-handler-interfaces.ko.md",
        "03-stream-connector.ko.md",
        "04-routing-id-allocation.ko.md",
        "05-route-mesh.ko.md",
        "06-location-store.ko.md",
        "dotnet-http-client.ko.md",
        "regression-test-matrix.ko.md",
        "backend-dependency-policy.ko.md",
    ];

    /// <summary>
    /// 언어마다 내용이 달라지는 장이다. `.NET` 디렉터리가 소유하고 앞뒤 장을 잇는
    /// adapter nav 마커를 가진다.
    /// </summary>
    private static readonly string[] LanguageGuideDocuments =
    [
        "11-monitoring.ko.md",
        "13-interface-catalog.ko.md",
        "16-options.ko.md"
    ];

    /// <summary>
    /// 모든 언어가 같은 내용을 공유하는 장이다. 공통 정본이 소유하며 언어별로 앞뒤가
    /// 달라지므로 adapter nav 마커를 두지 않는다. 장 번호는 언어에 상관없이 같은 식별자다.
    /// </summary>
    private static readonly string[] CommonGuideDocuments =
    [
        //  01은 dotnet 원문을 그대로 두고 코드와 언어별 사실만 탭으로 감쌌다.
        "01-overview.ko.md",
        "02-getting-started.ko.md",
        "03-concepts.ko.md",
        "04-backpressure.ko.md",
        "05-channel-messaging.ko.md",
        "06-spot.ko.md",
        "07-actor-spot.ko.md",
        "08-actor-session.ko.md",
        "09-stream.ko.md",
        "10-location.ko.md",
        "12-operations.ko.md",
        "14-samples.ko.md",
        "15-e2e-testing.ko.md",
        "17-alternative.ko.md"
    ];

    [Fact]
    public void DotNetContractDocuments_AllExposeRegressionTestSection()
    {
        var directory = GetDotNetDocRoot();
        var contractDirectory = GetDotNetContractDocRoot();
        // Narrative guide docs are onboarding prose, not contract docs, so they
        // are exempt from the regression-section requirement.
        // API examples remain tied to regression evidence and stay in the strict set.
        // 사용 가이드는 온보딩 산문이라 계약 문서가 아니다 — 세 패키지 가이드를 모두 제외한다.
        var excludedRoots = new[]
        {
            Path.Combine(directory, "guide"),
            // The reference tree is an unlisted preview and is not part of the
            // public contract document set until its ownership and nav entry are
            // finalized.
            Path.Combine(directory, "reference")
        };
        var actualDocuments = Directory
            .EnumerateFiles(directory, "*.ko.md", SearchOption.AllDirectories)
            .Where(path => !excludedRoots.Any(root => IsUnderDirectory(path, root, false)))
            .Where(path => !string.Equals(
                Path.GetFileName(path),
                "public-symbol-delta-v11.ko.md",
                StringComparison.Ordinal))
            .Concat(GetDotNetContractDocs()
                .Where(path => !string.Equals(
                    Path.GetFileName(path),
                    "README.ko.md",
                    StringComparison.Ordinal)))
            .Select(Path.GetFileName)
            .OfType<string>()
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(DotNetContractDocuments.Order(StringComparer.Ordinal), actualDocuments);

        var matrix = File.ReadAllText(ResolveDoc("regression-test-matrix.ko.md"));
        var references = ExtractRegressionTestReferences(matrix).ToArray();
        Assert.NotEmpty(references);
        Assert.DoesNotContain(
            references,
            static reference => reference.StartsWith("planned:", StringComparison.Ordinal));
    }

    [Fact]
    public void DotNetContractReadme_Exposes_Resolvable_Regression_Evidence()
    {
        var readme = File.ReadAllText(Path.Combine(GetDotNetContractDocRoot(), "README.ko.md"));

        Assert.Contains("## 회귀 테스트", readme, StringComparison.Ordinal);
        Assert.Contains(
            "ContractSurfaceCoverage.Fixed_spec_snapshot_matches_every_exported_contract_signature",
            readme,
            StringComparison.Ordinal);
        Assert.Contains(
            "RegressionTests.DotNetContractRegressionTestReferences_Resolve_ToActiveTestMethods",
            readme,
            StringComparison.Ordinal);
    }

    [Fact]
    public void DotNetGuideNarrative_DocumentsExist_AndAreWellFormed()
    {
        var guideRoot = Path.Combine(GetDotNetDocRoot(), "guide", "server");

        // 진입점 README는 읽는 순서만 담는 색인이라 장 목록에서 뺀다.
        var actual = Directory
            .EnumerateFiles(guideRoot, "*.ko.md", SearchOption.TopDirectoryOnly)
            .Select(Path.GetFileName)
            .OfType<string>()
            .Where(static name => !string.Equals(name, "README.ko.md", StringComparison.Ordinal))
            .Order(StringComparer.Ordinal)
            .ToArray();

        // 언어별 디렉터리는 직접 쓴 장과 공통 소스에서 생성한 장을 함께 담는다.
        Assert.Equal(
            LanguageGuideDocuments.Concat(CommonGuideDocuments).Order(StringComparer.Ordinal),
            actual);
        Assert.True(File.Exists(Path.Combine(guideRoot, "README.ko.md")),
            "언어별 가이드는 읽는 순서를 제시하는 진입점 README를 가진다.");

        foreach (var document in LanguageGuideDocuments.Concat(CommonGuideDocuments))
        {
            var text = File.ReadAllText(Path.Combine(guideRoot, document));
            Assert.Contains("<!-- framework-adapter-nav:start -->", text, StringComparison.Ordinal);
            Assert.Matches(@"(?m)^# .+", text);
        }

        // 생성판은 손으로 고치지 않는다. 표시가 없으면 사람이 직접 쓴 파일이라는 뜻이다.
        foreach (var document in CommonGuideDocuments)
        {
            var text = File.ReadAllText(Path.Combine(guideRoot, document));
            Assert.Contains("<!-- generated:start -->", text, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void CommonGuideNarrative_DocumentsExist_AndCarryNoLanguageNav()
    {
        var guideRoot = GetCommonGuideServerRoot();

        var actual = Directory
            .EnumerateFiles(guideRoot, "*.ko.md", SearchOption.TopDirectoryOnly)
            .Select(Path.GetFileName)
            .OfType<string>()
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(CommonGuideDocuments.Order(StringComparer.Ordinal), actual);

        foreach (var document in CommonGuideDocuments)
        {
            var text = File.ReadAllText(Path.Combine(guideRoot, document));
            Assert.Matches(@"(?m)^# .+", text);
            // 앞뒤 장은 언어별 진입점과 사이트 nav가 정한다.
            Assert.DoesNotContain("framework-adapter-nav", text, StringComparison.Ordinal);
            // 링크 대상이 한 언어의 문서를 가리키면 다른 언어의 독자에게 잘못된 계약을
            // 안내하게 된다. 탭 안은 이미 언어가 정해진 자리라 언어별 경로가 정상이므로
            // 탭 밖 산문만 본다.
            Assert.DoesNotMatch(
                @"\]\([^)]*languages/(dotnet|cpp|java|kotlin|node)/",
                OutsideLanguageTabs(text));
        }
    }

    /// <summary>
    /// 탭 블록(`=== "라벨"` 아래 들여쓴 줄)을 걷어낸 산문만 남긴다. 탭 안은 이미
    /// 언어가 정해진 자리라 언어별 이름과 경로가 정상이다.
    /// </summary>
    private static string OutsideLanguageTabs(string text)
    {
        var kept = new List<string>();
        var insideTab = false;
        foreach (var line in text.Split('\n'))
        {
            if (line.StartsWith("=== \"", StringComparison.Ordinal))
            {
                insideTab = true;
                continue;
            }

            if (insideTab)
            {
                if (line.Trim().Length == 0 || line.StartsWith("    ", StringComparison.Ordinal))
                {
                    continue;
                }

                insideTab = false;
            }

            kept.Add(line);
        }

        return string.Join('\n', kept);
    }

    /// <summary>
    /// 공통 정본은 코드가 스니펫이라 코드 층위는 체커가 보지만 산문은 아무도 대조하지
    /// 않는다. 그래서 챕터마다 계약을 소유하는 스펙 문서를 머리에 밝히고 그것과 맞춘다
    /// (런북 §11 게이트 4). 소유 문서가 없는 챕터는 없다고 밝힌다.
    /// </summary>
    [Fact]
    public void CommonGuideNarrative_DeclareTheSpecThatOwnsTheirContract()
    {
        // 계약이 아니라 안내가 본질인 챕터다. 소유 스펙이 없다고 밝히는 쪽이 맞다.
        var withoutOwningSpec = new[]
        {
            "14-samples.ko.md",
            "15-e2e-testing.ko.md",
            "17-alternative.ko.md",
        };

        foreach (var document in CommonGuideDocuments)
        {
            var text = File.ReadAllText(Path.Combine(GetCommonGuideServerRoot(), document));
            if (withoutOwningSpec.Contains(document, StringComparer.Ordinal))
            {
                Assert.Contains("이 장에는 계약을 소유하는 스펙 문서가 없다", text, StringComparison.Ordinal);
                continue;
            }

            Assert.Contains("**이 장의 계약 소유 문서**", text, StringComparison.Ordinal);
        }
    }

    /// <summary>
    /// 소스 코드 fence는 언어 탭 안에만 둔다. 탭 밖에 남으면 한 언어의 예제가 모든
    /// 언어의 독자에게 그대로 노출된다. mermaid·text·yaml처럼 언어에 중립인 블록은
    /// 탭 밖에 두어도 된다.
    /// </summary>
    [Fact]
    public void CommonGuideNarrative_SourceFences_LiveInsideLanguageTabs()
    {
        var sourceLanguages = new[]
        {
            "csharp", "cs", "cpp", "c++", "java", "kotlin", "typescript", "ts", "javascript", "js"
        };
        var offenders = new List<string>();

        foreach (var document in CommonGuideDocuments)
        {
            var path = Path.Combine(GetCommonGuideServerRoot(), document);
            var lines = File.ReadAllLines(path);

            for (var index = 0; index < lines.Length; index++)
            {
                // 탭 안의 fence는 4칸 들여쓴다 — 줄 첫 칸에서 시작하면 탭 밖이다.
                var line = lines[index];
                if (!line.StartsWith("```", StringComparison.Ordinal)) continue;
                var language = line[3..].Trim();
                if (!sourceLanguages.Contains(language, StringComparer.OrdinalIgnoreCase)) continue;
                offenders.Add($"{document}:{index + 1}: ```{language}");
            }
        }

        Assert.Empty(offenders.Order(StringComparer.Ordinal));
    }

    [Fact]
    public void DotNetDocs_SpotRouteChannelAcceptance_RulesStayDocumented()
    {
        var guideAndSampleDocs = Directory
            .EnumerateFiles(
                Path.Combine(GetDotNetDocRoot(), "guide"), "*.ko.md", SearchOption.AllDirectories)
            .Concat(Directory.EnumerateFiles(
                GetCommonGuideServerRoot(), "*.ko.md", SearchOption.AllDirectories))
            .ToArray();

        foreach (var path in guideAndSampleDocs)
        {
            var text = File.ReadAllText(path);
            Assert.DoesNotContain("AddChannel(", text, StringComparison.Ordinal);
            Assert.DoesNotContain("AddRouteChannel(", text, StringComparison.Ordinal);
        }

        // RouteMesh 등록과 peer 연결은 전용 언어 계약이 함께 소유한다.
        var routeMesh = File.ReadAllText(Path.Combine(
            GetDotNetContractDocRoot(),
            "interfaces",
            "03-configuration-topology.ko.md"));

        Assert.DoesNotContain("AcceptSpotRoutesFromChannel", routeMesh,
            StringComparison.Ordinal);
        Assert.Contains("AddRouteMesh", routeMesh,
            StringComparison.Ordinal);
        Assert.Contains("IZLinkMeshNodeBuilder", routeMesh,
            StringComparison.Ordinal);
        Assert.Contains("IZLinkMeshPeerConnections", routeMesh,
            StringComparison.Ordinal);
        Assert.DoesNotContain("NonPublic", routeMesh, StringComparison.Ordinal);
        Assert.DoesNotContain("internal/private", routeMesh,
            StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void DotNetDocs_BoundSession_Does_Not_Document_Request_Surface()
    {
        var docs = GetDotNetAndCommonGuideDocs();

        foreach (var path in docs)
        {
            var text = File.ReadAllText(path);
            Assert.DoesNotContain("IZLinkBoundSessionRequestCall", text, StringComparison.Ordinal);
            Assert.DoesNotContain("BoundSession.Request", text, StringComparison.Ordinal);
            Assert.DoesNotContain("Request<TRequest>(TRequest request)", text, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void DotNetDocs_DoNotDocument_Replaced_Spot_Address_Contracts()
    {
        var roots = new[] { GetDotNetDocRoot(), GetDotNetContractDocRoot(), GetCommonGuideServerRoot() };
        var forbidden = new[]
        {
            "IZLinkSpotRefResolver",
            "ResolveSpotRefAsync",
            "IZLinkActorAddressResolver",
            "ResolveActorSpotRefAsync",
            "IZLinkSpotLocationResolver"
        };

        foreach (var path in roots.SelectMany(root =>
                     Directory.EnumerateFiles(root, "*.ko.md", SearchOption.AllDirectories)))
        {
            var text = File.ReadAllText(path);
            foreach (var symbol in forbidden)
                Assert.DoesNotContain(symbol, text, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void DotNetDocs_DoNotDocumentNestedFrameworkConfigurationCallbacks()
    {
        var docRoot = GetDotNetDocRoot();
        var docs = GetDotNetAndCommonGuideDocs();
        var forbidden = new (Regex Pattern, string Reason)[]
        {
            (new Regex(@"\bEnable(?:Server|Client|Publisher|Subscriber)\s*\([\s\S]{0,160}?Action<", RegexOptions.Compiled),
                "nested capability callback"),
            (new Regex(@"\bUseManualConnections\s*\([\s\S]{0,160}?Action<", RegexOptions.Compiled),
                "manual connection callback"),
            (new Regex(@"\bAddNode\s*\([\s\S]{0,160}?Action<", RegexOptions.Compiled), "spot mesh node callback"),
            (new Regex(@"\bConfigureEntrySpot\s*\([\s\S]{0,160}?Action<", RegexOptions.Compiled),
                "entry spot options callback")
        };
        var offenders = new List<string>();

        foreach (var path in docs)
        {
            var text = File.ReadAllText(path);
            var relative = Path.GetRelativePath(docRoot, path);
            foreach (var (pattern, reason) in forbidden)
                if (pattern.IsMatch(text))
                    offenders.Add($"{relative}: {reason}");
        }

        Assert.Empty(offenders.Order(StringComparer.Ordinal));
    }

    [Fact]
    public void DotNetRegressionMatrix_References_AllContractDocuments()
    {
        var matrix = File.ReadAllText(ResolveDoc("regression-test-matrix.ko.md"));

        foreach (var document in DotNetContractDocuments) Assert.Contains(document, matrix, StringComparison.Ordinal);
    }

    [Fact]
    public void DotNetContractRegressionTestReferences_Resolve_ToActiveTestMethods()
    {
        var activeTests = GetActiveTestMethods();
        var activeE2EScenarios = GetActiveE2EScenarios();
        var unresolved = new List<string>();

        var document = "regression-test-matrix.ko.md";
        var references = ExtractRegressionTestReferences(File.ReadAllText(ResolveDoc(document))).ToArray();
        Assert.NotEmpty(references);

        foreach (var reference in references)
                if (reference.StartsWith("E2E:", StringComparison.Ordinal))
                {
                    if (!activeE2EScenarios.Contains(reference[4..]))
                        unresolved.Add($"{document}: {reference}");
                }
                else if (reference.StartsWith("SCRIPT:", StringComparison.Ordinal))
                {
                    var languageRoot = Path.GetFullPath(Path.Combine(
                        GetDotNetDocRoot(),
                        "..",
                        "..",
                        "..",
                        "languages",
                        "dotnet"));
                    if (!File.Exists(Path.Combine(languageRoot, reference[7..])))
                        unresolved.Add($"{document}: {reference}");
                }
                else if (!activeTests.Contains(reference))
                    unresolved.Add($"{document}: {reference}");

        Assert.True(
            unresolved.Count == 0,
            $"Unresolved regression references:{Environment.NewLine}{string.Join(Environment.NewLine, unresolved.Order(StringComparer.Ordinal))}");
    }

    [Fact]
    public void DotNetExactInterfaceDocuments_Have_An_Explicit_Regression_Owner()
    {
        var interfaceRoot = Path.Combine(GetDotNetContractDocRoot(), "interfaces");
        var matrix = File.ReadAllText(ResolveDoc("regression-test-matrix.ko.md"));
        const string owner = "ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports";

        var missing = Directory
            .EnumerateFiles(interfaceRoot, "*.ko.md", SearchOption.TopDirectoryOnly)
            .Where(path => !string.Equals(
                Path.GetFileName(path),
                "README.ko.md",
                StringComparison.Ordinal))
            .Where(path =>
            {
                var document = Path.GetFileName(path);
                return !Regex.IsMatch(
                    matrix,
                    $@"(?m)^\|\s*`{Regex.Escape(document)}`\s*\|.*`{Regex.Escape(owner)}`",
                    RegexOptions.CultureInvariant);
            })
            .Select(Path.GetFileName)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.True(
            missing.Length == 0,
            $"Exact interface documents without an explicit regression owner: {string.Join(", ", missing)}");
    }

    [Fact]
    public void DotNetRegressionMatrix_Includes_ExecutionSerialization_Guards()
    {
        var matrix = File.ReadAllText(ResolveDoc("regression-test-matrix.ko.md"));

        Assert.Contains("Entry Spot actor mailbox dispatch", matrix, StringComparison.Ordinal);
        Assert.Contains("local actor mailbox dispatch", matrix, StringComparison.Ordinal);
        Assert.Contains("user Spot actor dispatch serialization", matrix, StringComparison.Ordinal);
        Assert.Contains("session actor dispatch ordering", matrix, StringComparison.Ordinal);
        Assert.Contains("actor dispatch location after mailbox wait", matrix, StringComparison.Ordinal);
        Assert.Contains("session callback task dispatch", matrix, StringComparison.Ordinal);
        Assert.Contains("session callback 직렬성", matrix, StringComparison.Ordinal);
        Assert.Contains("runtime task exception observation", matrix, StringComparison.Ordinal);
        Assert.Contains("execution queue cancellation semantics", matrix, StringComparison.Ordinal);
    }

    [Fact]
    public void DotNetRegressionMatrix_Uses_ExplicitTurnTerminator_Contract()
    {
        var matrix = File.ReadAllText(ResolveDoc("regression-test-matrix.ko.md"));

        Assert.Contains("Explicit turn terminator regression", matrix, StringComparison.Ordinal);
        Assert.Contains(
            "RunCpuWorker_Async_Holds_Serial_Turn_Until_Work_Completes",
            matrix,
            StringComparison.Ordinal);
        Assert.Contains(
            "RunCpuWorker_Yield_Releases_And_Resumes_Through_Serial_Turn",
            matrix,
            StringComparison.Ordinal);
        Assert.Contains("E2E:ATD-B3", matrix, StringComparison.Ordinal);
        Assert.Contains("`Yield(...)`", matrix, StringComparison.Ordinal);
    }

    [Fact]
    public void EveryImplementedDotNetE2EConfigHasAnAllRunnerEntry()
    {
        var commonE2ERoot = Path.GetFullPath(Path.Combine(
            GetDotNetDocRoot(),
            "..",
            "common",
            "e2e"));
        var dotNetE2ERoot = Path.GetFullPath(Path.Combine(
            GetDotNetDocRoot(),
            "..",
            "..",
            "..",
            "languages",
            "dotnet",
            "e2e"));
        var fixtureByConfig = new Dictionary<int, string>
        {
            [1] = "LocationMessaging",
            [2] = "SpotService",
            [3] = "PubSub",
            [4] = "RegistrationCodec",
            [5] = "ResilienceLifecycle",
            [6] = "StoreFailure",
            [7] = "RuntimeMonitoring",
            [8] = "AutomaticTurnDispatch",
            [9] = "ToActorMessaging",
            [10] = "SpotActorTransfer",
            [11] = "ObservabilityOps"
        };
        var activeScenarios = GetActiveE2EScenarios();
        var allRunner = File.ReadAllText(Path.Combine(dotNetE2ERoot, "run_e2e_all.sh"));

        foreach (var pair in fixtureByConfig)
        {
            var document = Directory.GetFiles(
                    commonE2ERoot,
                    $"config-{pair.Key}-*.ko.md",
                    SearchOption.TopDirectoryOnly)
                .Single();
            var scenarioIds = Regex.Matches(
                    File.ReadAllText(document),
                    @"(?m)^#{2,5}\s+(?<id>[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)+)\b")
                .Select(static match => match.Groups["id"].Value)
                .ToArray();

            Assert.NotEmpty(scenarioIds);

            // 공통 E2E 문서는 target-first 검증 기준이다. 아직 구현되지 않은 scenario는
            // feature-map의 gap으로 남을 수 있으며, 이 테스트는 active fixture와 runner 배선을 검증한다.
            Assert.Contains(scenarioIds, activeScenarios.Contains);
            Assert.Single(Regex.Matches(
                    allRunner,
                    $@"(?m)^\s{{2}}{Regex.Escape(pair.Value)}$")
                .Cast<Match>());
        }

    }

    [Fact]
    public void EveryCommonE2EConfigHasAnExplicitAggregateRunnerEntry()
    {
        var dotNetE2ERoot = Path.GetFullPath(Path.Combine(
            GetDotNetDocRoot(),
            "..",
            "..",
            "..",
            "languages",
            "dotnet",
            "e2e"));
        var allRunner = File.ReadAllText(Path.Combine(
            dotNetE2ERoot,
            "run_e2e_all.sh"));
        var configs = new Dictionary<int, string>
        {
            [1] = "LocationMessaging",
            [2] = "SpotService",
            [3] = "PubSub",
            [4] = "RegistrationCodec",
            [5] = "ResilienceLifecycle",
            [6] = "StoreFailure",
            [7] = "RuntimeMonitoring",
            [8] = "AutomaticTurnDispatch",
            [9] = "ToActorMessaging",
            [10] = "SpotActorTransfer",
            [11] = "ObservabilityOps",
            [12] = "ChannelEgressRouting",
            [13] = "SubmitAdmission",
            [14] = "InstanceSpot"
        };

        foreach (var (number, name) in configs)
        {
            var directory = Path.Combine(dotNetE2ERoot, name);
            Assert.True(
                File.Exists(Path.Combine(directory, "feature-map.ko.md")),
                $"Config {number} has no .NET feature map.");
            Assert.True(
                File.Exists(Path.Combine(directory, "run_e2e.sh")),
                $"Config {number} has no process runner.");
            Assert.Single(Regex.Matches(
                    allRunner,
                    $@"(?m)^\s{{2}}{Regex.Escape(name)}$")
                .Cast<Match>());
        }
    }

    [Fact]
    public void CommonE2EConfigsHaveCompleteDotNetFeatureMapInventories()
    {
        var commonE2ERoot = Path.GetFullPath(Path.Combine(
            GetDotNetDocRoot(),
            "..",
            "common",
            "e2e"));
        var dotNetE2ERoot = Path.GetFullPath(Path.Combine(
            GetDotNetDocRoot(),
            "..",
            "..",
            "..",
            "languages",
            "dotnet",
            "e2e"));
        var fixtureByConfig = new Dictionary<int, string>
        {
            [1] = "LocationMessaging",
            [2] = "SpotService",
            [3] = "PubSub",
            [4] = "RegistrationCodec",
            [5] = "ResilienceLifecycle",
            [6] = "StoreFailure",
            [7] = "RuntimeMonitoring",
            [8] = "AutomaticTurnDispatch",
            [9] = "ToActorMessaging",
            [10] = "SpotActorTransfer",
            [11] = "ObservabilityOps",
            [12] = "ChannelEgressRouting",
            [13] = "SubmitAdmission",
            [14] = "InstanceSpot"
        };

        foreach (var pair in fixtureByConfig)
        {
            var document = Directory.GetFiles(
                    commonE2ERoot,
                    $"config-{pair.Key}-*.ko.md",
                    SearchOption.TopDirectoryOnly)
                .Single();
            var featureMap = Path.Combine(
                dotNetE2ERoot,
                pair.Value,
                "feature-map.ko.md");
            Assert.True(
                File.Exists(featureMap),
                $"Missing .NET feature map for Config {pair.Key}: {featureMap}");

            var documentText = File.ReadAllText(document);
            var documentIds = Regex.Matches(
                    documentText,
                    @"(?m)^(?:#{2,5}\s+(?<id>[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)+)\b|\|\s+`?(?<tableId>CH-REG-[0-9]+)`?\s+\|)")
                .Select(static match =>
                    match.Groups["id"].Success
                        ? match.Groups["id"].Value
                        : match.Groups["tableId"].Value)
                .ToArray();
            var scenarioPrefix = pair.Key switch
            {
                1 => "RM-",
                2 => "SM-",
                3 => "PS-",
                4 => "RC-",
                5 => "RL-",
                6 => "SF-",
                7 => "MON-",
                8 => "TD-",
                9 => "TA-",
                10 => "ST-",
                11 => "OBS-",
                12 => "CH-E2E-",
                13 => "SA-E2E-",
                14 => "IS-E2E-",
                _ => throw new InvalidOperationException($"Unsupported Config {pair.Key}.")
            };
            var referencedScenarioIds = Regex.Matches(
                    documentText,
                    $@"\b{Regex.Escape(scenarioPrefix)}[A-Z0-9]+(?:-[A-Z0-9]+)*\b")
                .Select(static match => match.Value)
                .ToHashSet(StringComparer.Ordinal);
            var featureMapIds = Regex.Matches(
                    File.ReadAllText(featureMap),
                    @"(?m)^\| (?<id>[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)+) \|")
                .Select(static match => match.Groups["id"].Value)
                .ToArray();
            var duplicateDocumentIds = documentIds
                .GroupBy(static id => id, StringComparer.Ordinal)
                .Where(static group => group.Count() > 1)
                .Select(static group => group.Key)
                .Order(StringComparer.Ordinal)
                .ToArray();
            var duplicateFeatureMapIds = featureMapIds
                .GroupBy(static id => id, StringComparer.Ordinal)
                .Where(static group => group.Count() > 1)
                .Select(static group => group.Key)
                .Order(StringComparer.Ordinal)
                .ToArray();
            var expectedIds = documentIds
                .Concat(referencedScenarioIds)
                .ToHashSet(StringComparer.Ordinal)
                .ToArray();
            var missing = expectedIds
                .Except(featureMapIds, StringComparer.Ordinal)
                .Order(StringComparer.Ordinal)
                .ToArray();
            var unknown = featureMapIds
                .Where(id => id.StartsWith(scenarioPrefix, StringComparison.Ordinal))
                .ToHashSet(StringComparer.Ordinal)
                .Except(expectedIds, StringComparer.Ordinal)
                .Order(StringComparer.Ordinal)
                .ToArray();

            Assert.True(
                duplicateDocumentIds.Length == 0,
                $"Config {pair.Key} common E2E document has duplicate scenario IDs: "
                + string.Join(", ", duplicateDocumentIds));
            Assert.True(
                duplicateFeatureMapIds.Length == 0,
                $"Config {pair.Key} {pair.Value} feature map has duplicate scenario IDs: "
                + string.Join(", ", duplicateFeatureMapIds));
            Assert.True(
                missing.Length == 0,
                $"Config {pair.Key} scenarios missing from {pair.Value} feature map: "
                + string.Join(", ", missing));
            Assert.True(
                unknown.Length == 0,
                $"Config {pair.Key} {pair.Value} feature map has unknown scenario IDs: "
                + string.Join(", ", unknown));
        }
    }

    [Fact]
    public void SpotNodeContractsUseTheLocationStoreAsTheAddressSourceOfTruth()
    {
        // Location Store 요구 조건은 언어별 configuration exact interface가 소유한다.
        var dotnet = File.ReadAllText(Path.Combine(
            GetDotNetContractDocRoot(),
            "interfaces",
            "03-configuration-topology.ko.md"));
        var node = File.ReadAllText(Path.GetFullPath(Path.Combine(
            GetDotNetContractDocRoot(),
            "..",
            "node",
            "01-system-structure.ko.md")));

        Assert.Contains("location store", dotnet, StringComparison.Ordinal);
        Assert.Contains("location store", node, StringComparison.Ordinal);
        Assert.DoesNotContain("core `ResolveSpot", dotnet, StringComparison.Ordinal);
        Assert.DoesNotContain("core `resolveSpot", node, StringComparison.Ordinal);
    }

    [Fact]
    public void DotNetLanguageContractsPreserveTheReviewedPublicRuntimeDecisions()
    {
        var handlers = File.ReadAllText(Path.Combine(
            GetDotNetContractDocRoot(),
            "interfaces",
            "06-actors.ko.md"));
        var system = File.ReadAllText(Path.Combine(
            GetDotNetContractDocRoot(),
            "interfaces",
            "02-configuration-host.ko.md"));
        var routeMesh = File.ReadAllText(Path.Combine(
            GetDotNetContractDocRoot(),
            "interfaces",
            "03-configuration-topology.ko.md"));
        var locationStore = File.ReadAllText(Path.Combine(
            GetDotNetContractDocRoot(),
            "interfaces",
            "08-authority-relocation.ko.md"));

        Assert.Contains("public interface IZLinkMeshNodeBuilder", routeMesh, StringComparison.Ordinal);
        Assert.Contains("public interface IZLinkMeshPeerConnections", routeMesh, StringComparison.Ordinal);
        Assert.Contains("string? packetName = null", routeMesh, StringComparison.Ordinal);
        Assert.Contains("`ZLinkConfigurationException`", system, StringComparison.Ordinal);
        Assert.Contains("ASP.NET Core", system, StringComparison.Ordinal);
        Assert.Contains("public interface IZLinkActorClient", handlers, StringComparison.Ordinal);
        Assert.Contains("public interface IZLinkLocationStore", locationStore, StringComparison.Ordinal);
    }

    [Fact]
    public void CanonicalCommonSpecOwnsLiveDotNetContracts()
    {
        var specRoot = GetCommonSpecRoot();
        var deletedSpecRoot = Path.GetFullPath(Path.Combine(specRoot, "..", "..", "spec"));
        var required = new[]
        {
            "07-channel-topology.ko.md",
            "14-actor-model.ko.md",
            "20-session-actor-dispatch.ko.md",
            "29-transport-liveness.ko.md"
        };
        var writingGuide = Path.GetFullPath(Path.Combine(
            specRoot,
            "..",
            "..",
            "..",
            "..",
            "..",
            "doc",
            "principal",
            "documentation",
            "spec-writing-guide.ko.md"));

        Assert.False(Directory.Exists(deletedSpecRoot));
        Assert.All(required, file => Assert.True(File.Exists(Path.Combine(specRoot, file)), file));
        Assert.True(File.Exists(writingGuide), writingGuide);
        Assert.False(File.Exists(Path.Combine(
            specRoot,
            "00-spec-writing-guide.ko.md")));
        Assert.True(Directory.Exists(Path.Combine(GetDotNetContractDocRoot(), "interfaces")));
    }

    private static string GetDotNetDocRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);

        while (current is not null)
        {
            var candidate = Path.Combine(
                current.FullName,
                "framework",
                "doc",
                "framework",
                "dotnet");

            if (Directory.Exists(candidate)) return candidate;

            current = current.Parent;
        }

        throw new DirectoryNotFoundException(
            "Could not find framework/doc/framework/dotnet from test runtime.");
    }

    /// <summary>
    /// `.NET`의 공개 계약 문서는 framework server, HTTP client와 stream connector에 걸쳐 있다.
    /// </summary>
    private static string[] GetDotNetContractDocs()
    {
        var connectorRoot = Path.Combine(
            GetCommonSpecRoot(),
            "stream-connector",
            "languages",
            "dotnet");
        var httpClientRoot = Path.Combine(
            GetCommonSpecRoot(),
            "http-client",
            "languages",
            "dotnet");

        return Directory
            .EnumerateFiles(GetDotNetContractDocRoot(), "*.ko.md", SearchOption.TopDirectoryOnly)
            .Concat(Directory.EnumerateFiles(connectorRoot, "*.ko.md", SearchOption.TopDirectoryOnly))
            .Concat(Directory.EnumerateFiles(httpClientRoot, "*.ko.md", SearchOption.TopDirectoryOnly))
            .Order(StringComparer.Ordinal)
            .ToArray();
    }

    private static string GetCommonSpecRoot()
    {
        return Path.GetFullPath(Path.Combine(GetDotNetDocRoot(), "..", "common", "spec"));
    }

    /// <summary>
    /// 모든 언어가 공유하는 가이드 정본이다. `.NET` 문서 트리 밖에 있으므로 금칙 심볼
    /// 검사가 이 디렉터리를 따로 포함해야 한다.
    /// </summary>
    private static string GetCommonGuideServerRoot()
    {
        return Path.GetFullPath(Path.Combine(
            GetDotNetDocRoot(), "..", "common", "guide", "server"));
    }

    /// <summary>
    /// 금칙 심볼 검사가 훑는 산문 문서 전체다 — `.NET` 문서 트리와 공통 가이드 정본.
    /// </summary>
    private static string[] GetDotNetAndCommonGuideDocs()
    {
        return Directory
            .EnumerateFiles(GetDotNetDocRoot(), "*.ko.md", SearchOption.AllDirectories)
            .Concat(Directory.EnumerateFiles(
                GetCommonGuideServerRoot(), "*.ko.md", SearchOption.AllDirectories))
            .Where(static path => !path.Contains(
                $"{Path.DirectorySeparatorChar}draft{Path.DirectorySeparatorChar}",
                StringComparison.Ordinal))
            .Order(StringComparer.Ordinal)
            .ToArray();
    }

    private static string GetDotNetContractDocRoot()
    {
        var dotnetDocRoot = GetDotNetDocRoot();
        var contractRoot = Path.GetFullPath(Path.Combine(
            dotnetDocRoot,
            "..",
            "common",
            "spec",
            "server",
            "languages",
            "dotnet"));

        return Directory.Exists(contractRoot)
            ? contractRoot
            : throw new DirectoryNotFoundException(
                "Could not find framework/doc/framework/common/spec/server/languages/dotnet from test runtime.");
    }

    private static string ResolveDoc(string fileName)
    {
        var matches = Directory
            .EnumerateFiles(GetDotNetDocRoot(), fileName, SearchOption.AllDirectories)
            .Concat(Directory.EnumerateFiles(
                GetDotNetContractDocRoot(),
                fileName,
                SearchOption.TopDirectoryOnly))
            .Where(path => !string.Equals(
                Path.GetFileName(path),
                "README.ko.md",
                StringComparison.Ordinal)
                || string.Equals(
                    Path.GetDirectoryName(path),
                    GetDotNetDocRoot(),
                    StringComparison.Ordinal))
            .ToArray();

        return matches.Length switch
        {
            1 => matches[0],
            0 => throw new FileNotFoundException(
                $"Could not find .NET document '{fileName}'."),
            _ => throw new InvalidOperationException(
                $"Ambiguous document '{fileName}': {string.Join(", ", matches)}")
        };
    }

    private static IReadOnlyCollection<string> ExtractRegressionTestReferences(string text)
    {
        var sectionMatch = Regex.Match(
            text,
            @"^## [^\r\n]*회귀 테스트[^\r\n]*\r?\n(?<body>.*?)(?=^## |\z)",
            RegexOptions.Multiline | RegexOptions.Singleline);

        Assert.True(sectionMatch.Success, "Missing regression test section.");

        return Regex
            .Matches(
                sectionMatch.Groups["body"].Value,
                @"^\| (?<cell>.*?) \|",
                RegexOptions.Multiline)
            .SelectMany(static row => Regex
                .Matches(row.Groups["cell"].Value, @"`(?<test>[^`]+)`")
                .Select(static match => match.Groups["test"].Value)
                .Where(static value => !value.EndsWith(".ko.md", StringComparison.OrdinalIgnoreCase)))
            .ToArray();
    }

    private static IReadOnlySet<string> GetActiveTestMethods()
    {
        var testsRoot = GetTestsRoot();
        var sourceFiles = new HashSet<string>(StringComparer.Ordinal);

        foreach (var projectPath in Directory.EnumerateFiles(testsRoot, "*.csproj", SearchOption.AllDirectories))
            AddProjectSources(projectPath, sourceFiles);

        var activeTests = new HashSet<string>(StringComparer.Ordinal);
        foreach (var sourceFile in sourceFiles)
        {
            var text = File.ReadAllText(sourceFile);
            var classMatches = Regex.Matches(
                text,
                @"(?m)^\s*(?:(?:public|internal|file|private|protected)\s+)*(?:(?:sealed|static|abstract|partial|new)\s+)*class\s+(?<class>[A-Za-z_][A-Za-z0-9_]*)\b");

            if (classMatches.Count == 0) continue;

            foreach (Match methodMatch in Regex.Matches(
                         text,
                         @"\bpublic\s+(?:async\s+)?(?:Task|void)\s+(?<method>[A-Za-z_][A-Za-z0-9_]*)\s*\("))
            {
                var className = classMatches
                    .Last(match => match.Index < methodMatch.Index)
                    .Groups["class"].Value;
                var methodName = methodMatch.Groups["method"].Value;
                if (HasFactOrTheoryAttribute(text, methodMatch.Index)) activeTests.Add($"{className}.{methodName}");
            }
        }

        return activeTests;
    }

    private static IReadOnlySet<string> GetActiveE2EScenarios()
    {
        var e2eRoot = Path.GetFullPath(Path.Combine(
            GetDotNetDocRoot(),
            "..",
            "..",
            "..",
            "languages",
            "dotnet",
            "e2e"));
        var scenarios = new HashSet<string>(StringComparer.Ordinal);
        foreach (var featureMap in Directory.EnumerateFiles(
                     e2eRoot,
                     "feature-map.ko.md",
                     SearchOption.AllDirectories))
        {
            var text = File.ReadAllText(featureMap);
            var runnerPath = Path.Combine(
                Path.GetDirectoryName(featureMap)
                ?? throw new InvalidOperationException($"Missing feature-map directory: {featureMap}"),
                "run_e2e.sh");
            if (!File.Exists(runnerPath)) continue;
            var fixtureRoot = Path.GetDirectoryName(featureMap)!;
            var implementationText = string.Join(
                Environment.NewLine,
                Directory
                    .EnumerateFiles(fixtureRoot, "*", SearchOption.AllDirectories)
                    .Where(static path => path.EndsWith(".cs", StringComparison.Ordinal)
                                          || path.EndsWith(".sh", StringComparison.Ordinal))
                    .Where(static path => !path.Contains(
                                              $"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                              StringComparison.Ordinal)
                                          && !path.Contains(
                                              $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                              StringComparison.Ordinal))
                    .Select(File.ReadAllText));
            foreach (Match match in Regex.Matches(
                         text,
                         @"(?m)^\| (?<id>[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)+) \| (?<status>[^|]+) \|"))
            {
                var scenarioId = match.Groups["id"].Value;
                if (match.Groups["status"].Value.Trim() == "구현"
                    && implementationText.Contains(scenarioId, StringComparison.OrdinalIgnoreCase))
                    scenarios.Add(scenarioId);
            }
        }

        // The read-only regression matrix still names the two pre-TD Config 8
        // proofs. Resolve those historical proof labels to their canonical TD
        // fixtures until that document is revised in its own documentation task.
        if (scenarios.Contains("TD-A1")) scenarios.Add("ATD-A1");
        if (scenarios.Contains("TD-E1")) scenarios.Add("ATD-B3");

        return scenarios;
    }

    private static IEnumerable<string> ExtractLedgerProofReferences(string ledger)
    {
        foreach (var line in ledger.Split('\n'))
        {
            if (!line.StartsWith("| DN-", StringComparison.Ordinal)) continue;
            var cells = line.Split('|');
            if (cells.Length < 6) continue;
            var proofCell = cells[^2];
            foreach (Match match in Regex.Matches(proofCell, @"`(?<reference>[^`]+)`"))
            {
                var reference = match.Groups["reference"].Value;
                if (reference.StartsWith("E2E:", StringComparison.Ordinal)
                    || Regex.IsMatch(
                        reference,
                        @"^[A-Za-z_][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*$"))
                    yield return reference;
            }
        }
    }

    private static bool HasFactOrTheoryAttribute(string text, int methodIndex)
    {
        var prefix = text[..methodIndex];
        var factIndex = new[] { "[Fact", "[Theory", "[SkippableFact", "[SkippableTheory" }
            .Max(attribute => prefix.LastIndexOf(attribute, StringComparison.Ordinal));
        if (factIndex < 0) return false;

        var priorMethod = Regex.Matches(
                prefix,
                @"\bpublic\s+(?:async\s+)?(?:Task|void)\s+[A-Za-z_][A-Za-z0-9_]*\s*\(")
            .Cast<Match>()
            .LastOrDefault();
        if (priorMethod is not null && factIndex < priorMethod.Index) return false;

        var attributes = prefix[factIndex..];
        return !Regex.IsMatch(attributes, @"\bSkip\s*=");
    }

    private static bool IsUnderDirectory(
        string path,
        string directory,
        bool includeDirectChildrenOnly)
    {
        var actualDirectory = Path.GetDirectoryName(path);
        if (actualDirectory is null) return false;

        if (includeDirectChildrenOnly) return string.Equals(actualDirectory, directory, StringComparison.Ordinal);

        var relative = Path.GetRelativePath(directory, actualDirectory);
        return relative == "."
               || (!relative.StartsWith("..", StringComparison.Ordinal)
                   && !Path.IsPathRooted(relative));
    }

    private static void AddProjectSources(string projectPath, ISet<string> sourceFiles)
    {
        var projectDirectory = Path.GetDirectoryName(projectPath)
                               ?? throw new InvalidOperationException(
                                   $"Could not get project directory for '{projectPath}'.");
        var document = XDocument.Load(projectPath);
        var defaultCompileItems = document
            .Descendants("EnableDefaultCompileItems")
            .LastOrDefault()?.Value;
        var projectSources = string.Equals(defaultCompileItems, "false", StringComparison.OrdinalIgnoreCase)
            ? new HashSet<string>(StringComparer.Ordinal)
            : Directory
                .EnumerateFiles(projectDirectory, "*.cs", SearchOption.AllDirectories)
                .Where(static path => !path.Contains(
                                          $"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                          StringComparison.Ordinal)
                                      && !path.Contains(
                                          $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                          StringComparison.Ordinal))
                .Select(Path.GetFullPath)
                .ToHashSet(StringComparer.Ordinal);

        foreach (var remove in document.Descendants("Compile")
                     .SelectMany(static element => element.Attributes("Remove")))
        foreach (var removedPath in ResolveProjectPattern(projectDirectory, remove.Value))
            projectSources.Remove(removedPath);

        foreach (var include in document.Descendants("Compile")
                     .SelectMany(static element => element.Attributes("Include")))
        foreach (var includedPath in ResolveProjectPattern(projectDirectory, include.Value))
            projectSources.Add(includedPath);

        foreach (var sourceFile in projectSources) sourceFiles.Add(sourceFile);
    }

    private static IEnumerable<string> ResolveProjectPattern(string projectDirectory, string pattern)
    {
        if (pattern.Contains('*', StringComparison.Ordinal)) yield break;

        var path = Path.GetFullPath(Path.Combine(projectDirectory, pattern));
        if (File.Exists(path)) yield return path;
    }

    private static string GetTestsRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);

        while (current is not null)
        {
            var candidate = Path.Combine(current.FullName, "framework", "languages", "dotnet", "tests");
            if (Directory.Exists(candidate)) return candidate;

            current = current.Parent;
        }

        throw new DirectoryNotFoundException(
            "Could not find framework/languages/dotnet/tests from test runtime.");
    }
}
