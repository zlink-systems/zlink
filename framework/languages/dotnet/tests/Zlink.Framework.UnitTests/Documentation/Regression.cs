using System.Text.RegularExpressions;
using System.Xml.Linq;

namespace Zlink.Framework.UnitTests.Documentation;

public sealed class RegressionTests
{
    private static readonly string[] ExcludedFileNames =
    [
        "public-symbol-delta-v11.ko.md",
        "quickstart.ko.md",
    ];

    private static readonly string[] DotNetContractDocuments =
    [
        "README.ko.md",
        // 언어별 공개 계약은 시스템 구조, 인터페이스와 선택 capability의 정확한 시그니처를 고정한다.
        // 기능별 의미는 공통 스펙(framework/doc/framework/common/spec)이 소유한다.
        // 내용 없이 링크만 중계하던 이동 안내 문서는 지웠다 — 정확한 선언은
        // languages/dotnet/interfaces/ 아래 범주별 문서가 소유한다.
        "03-stream-connector.ko.md",
        "dotnet-http-client.ko.md",
    ];

    /// <summary>
    /// 언어마다 내용이 달라지는 장이다. `.NET` 디렉터리가 소유하고 앞뒤 장을 잇는
    /// adapter nav 마커를 가진다.
    /// </summary>
    private static readonly string[] LanguageGuideDocuments = ["13-interface-catalog.ko.md"];

    /// <summary>
    /// 모든 언어가 같은 내용을 공유하는 장이다. 공통 정본이 소유하며 언어별로 앞뒤가
    /// 달라지므로 adapter nav 마커를 두지 않는다. 장 번호는 언어에 상관없이 같은 식별자다.
    /// </summary>
    private static readonly string[] CommonGuideDocuments =
    [
        //  01은 dotnet 원문을 그대로 두고 코드와 언어별 사실만 탭으로 감쌌다.
        "01-overview.ko.md",
        "03-concepts.ko.md",
        "12-operations.ko.md",
        "14-samples.ko.md",
        "15-e2e-testing.ko.md",
        "16-options.ko.md",
        "17-alternative.ko.md",
        "20-channel-messaging.ko.md",
        "21-spot.ko.md",
        "22-actor.ko.md",
        "23-stream.ko.md",
        "24-actor-session.ko.md",
        "25-location.ko.md",
        "26-monitoring.ko.md",
        "30-channel-patterns.ko.md",
        "31-handler-dispatch.ko.md",
        "32-execution-model.ko.md",
        "33-backpressure.ko.md",
        "34-activation-lifetime.ko.md",
        "35-actor-membership.ko.md",
        "36-timer-worker.ko.md",
        "37-relocation.ko.md",
        "38-stream-boundary.ko.md",
        "39-session-binding.ko.md",
        //  샘플 따라 읽기 장. 계약은 common/sample이 소유하므로 소유 스펙 머리말을 두지 않는다.
        "50-bingo.ko.md",
        "51-tictactoe.ko.md",
        "52-supportchat.ko.md",
        "53-deliverydispatch.ko.md",
        "54-shoppingmall.ko.md",
        "55-gamequest.ko.md",
        "56-zoneworld.ko.md",
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
            // Internals documents record implementation decisions and E2E
            // evidence; they are not public contract documents and do not
            // carry the contract regression-section requirement.
            Path.Combine(directory, "internals"),
            // The reference tree is an unlisted preview and is not part of the
            // public contract document set until its ownership and nav entry are
            // finalized.
            Path.Combine(directory, "reference"),
        };
        var actualDocuments = Directory
            .EnumerateFiles(directory, "*.ko.md", SearchOption.AllDirectories)
            .Where(path => !excludedRoots.Any(root => IsUnderDirectory(path, root, false)))
            // The quickstart is onboarding prose like the guide, not a contract
            // document; it carries no regression-test section.
            .Where(path =>
                !ExcludedFileNames.Contains(Path.GetFileName(path), StringComparer.Ordinal)
            )
            .Concat(
                GetDotNetContractDocs()
                    .Where(path =>
                        !string.Equals(
                            Path.GetFileName(path),
                            "README.ko.md",
                            StringComparison.Ordinal
                        )
                    )
            )
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
            static reference => reference.StartsWith("planned:", StringComparison.Ordinal)
        );
    }

    [Fact]
    public void DotNetContractReadme_Exposes_Resolvable_Regression_Evidence()
    {
        var readme = File.ReadAllText(Path.Combine(GetDotNetContractDocRoot(), "README.ko.md"));

        Assert.Contains("## 회귀 테스트", readme, StringComparison.Ordinal);
        Assert.Contains(
            "ContractSurfaceCoverage.Fixed_spec_snapshot_matches_every_exported_contract_signature",
            readme,
            StringComparison.Ordinal
        );
        Assert.Contains(
            "RegressionTests.DotNetContractRegressionTestReferences_Resolve_ToActiveTestMethods",
            readme,
            StringComparison.Ordinal
        );
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
            actual
        );
        Assert.True(
            File.Exists(Path.Combine(guideRoot, "README.ko.md")),
            "언어별 가이드는 읽는 순서를 제시하는 진입점 README를 가진다."
        );

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
                OutsideLanguageTabs(text)
            );
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
    /// 두 층으로 다시 쓰는 장이다(`doc/plan/guide-rewrite.ko.md`). 작성 가이드 §1.4가
    /// "독자를 스펙으로 내보내지 않는다"를 정하므로 이 장들은 소유 스펙 머리말을 두지
    /// 않는다. 옛 형식의 장과 섞이지 않게 여기서 갈라 둔다.
    /// </summary>
    private static readonly string[] RewriteLayerGuideDocuments =
    [
        "20-channel-messaging.ko.md",
        "21-spot.ko.md",
        "22-actor.ko.md",
        "23-stream.ko.md",
        "24-actor-session.ko.md",
        "25-location.ko.md",
        "30-channel-patterns.ko.md",
        "31-handler-dispatch.ko.md",
        "32-execution-model.ko.md",
        "34-activation-lifetime.ko.md",
        "35-actor-membership.ko.md",
        "36-timer-worker.ko.md",
        "37-relocation.ko.md",
        "38-stream-boundary.ko.md",
        "39-session-binding.ko.md",
        "26-monitoring.ko.md",
        "01-overview.ko.md",
        "03-concepts.ko.md",
        "16-options.ko.md",
        "17-alternative.ko.md",
    ];

    /// <summary>
    /// 공통 정본은 코드가 스니펫이라 코드 층위는 체커가 보지만 산문은 아무도 대조하지
    /// 않는다. 그래서 옛 형식의 챕터는 계약을 소유하는 스펙 문서를 머리에 밝히고 그것과
    /// 맞춘다(런북 §11 게이트 4). 소유 문서가 없는 챕터는 없다고 밝힌다. 다시 쓰는 층의
    /// 장은 반대로 그 머리말을 두지 않는다.
    /// </summary>
    [Fact]
    public void CommonGuideNarrative_DeclareTheSpecThatOwnsTheirContract()
    {
        // 계약이 아니라 안내가 본질인 챕터다. 소유 스펙이 없다고 밝히는 쪽이 맞다.
        var withoutOwningSpec = new[]
        {
            "14-samples.ko.md",
            "15-e2e-testing.ko.md",
            "50-bingo.ko.md",
            "51-tictactoe.ko.md",
            "52-supportchat.ko.md",
            "53-deliverydispatch.ko.md",
            "54-shoppingmall.ko.md",
            "55-gamequest.ko.md",
            "56-zoneworld.ko.md",
        };

        foreach (var document in CommonGuideDocuments)
        {
            var text = File.ReadAllText(Path.Combine(GetCommonGuideServerRoot(), document));
            if (RewriteLayerGuideDocuments.Contains(document, StringComparer.Ordinal))
            {
                Assert.DoesNotContain("**이 장의 계약 소유 문서**", text, StringComparison.Ordinal);
                continue;
            }

            if (withoutOwningSpec.Contains(document, StringComparer.Ordinal))
            {
                Assert.Contains(
                    "이 장에는 계약을 소유하는 스펙 문서가 없다",
                    text,
                    StringComparison.Ordinal
                );
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
            "csharp",
            "cs",
            "cpp",
            "c++",
            "java",
            "kotlin",
            "typescript",
            "ts",
            "javascript",
            "js",
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
                if (!line.StartsWith("```", StringComparison.Ordinal))
                    continue;
                var language = line[3..].Trim();
                if (!sourceLanguages.Contains(language, StringComparer.OrdinalIgnoreCase))
                    continue;
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
                Path.Combine(GetDotNetDocRoot(), "guide"),
                "*.ko.md",
                SearchOption.AllDirectories
            )
            .Concat(
                Directory.EnumerateFiles(
                    GetCommonGuideServerRoot(),
                    "*.ko.md",
                    SearchOption.AllDirectories
                )
            )
            .ToArray();

        foreach (var path in guideAndSampleDocs)
        {
            var text = File.ReadAllText(path);
            Assert.DoesNotContain("AddChannel(", text, StringComparison.Ordinal);
            Assert.DoesNotContain("AddRouteChannel(", text, StringComparison.Ordinal);
        }

        // RouteMesh 등록과 peer 연결은 전용 언어 계약이 함께 소유한다.
        var routeMesh = File.ReadAllText(
            Path.Combine(
                GetDotNetContractDocRoot(),
                "interfaces",
                "03-configuration-topology.ko.md"
            )
        );

        Assert.DoesNotContain("AcceptSpotRoutesFromChannel", routeMesh, StringComparison.Ordinal);
        Assert.Contains("AddRouteMesh", routeMesh, StringComparison.Ordinal);
        Assert.Contains("IZLinkMeshNodeBuilder", routeMesh, StringComparison.Ordinal);
        Assert.Contains("IZLinkMeshPeerConnections", routeMesh, StringComparison.Ordinal);
        Assert.DoesNotContain("NonPublic", routeMesh, StringComparison.Ordinal);
        Assert.DoesNotContain("internal/private", routeMesh, StringComparison.OrdinalIgnoreCase);
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
            Assert.DoesNotContain(
                "Request<TRequest>(TRequest request)",
                text,
                StringComparison.Ordinal
            );
        }
    }

    [Fact]
    public void DotNetDocs_DoNotDocument_Replaced_Spot_Address_Contracts()
    {
        var roots = new[]
        {
            GetDotNetDocRoot(),
            GetDotNetContractDocRoot(),
            GetCommonGuideServerRoot(),
        };
        var forbidden = new[]
        {
            "IZLinkSpotRefResolver",
            "ResolveSpotRefAsync",
            "IZLinkActorAddressResolver",
            "ResolveActorSpotRefAsync",
            "IZLinkSpotLocationResolver",
        };

        foreach (
            var path in roots.SelectMany(root =>
                Directory.EnumerateFiles(root, "*.ko.md", SearchOption.AllDirectories)
            )
        )
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
            (
                new Regex(
                    @"\bEnable(?:Server|Client|Publisher|Subscriber)\s*\([\s\S]{0,160}?Action<",
                    RegexOptions.Compiled
                ),
                "nested capability callback"
            ),
            (
                new Regex(
                    @"\bUseManualConnections\s*\([\s\S]{0,160}?Action<",
                    RegexOptions.Compiled
                ),
                "manual connection callback"
            ),
            (
                new Regex(@"\bAddNode\s*\([\s\S]{0,160}?Action<", RegexOptions.Compiled),
                "spot mesh node callback"
            ),
            (
                new Regex(@"\bConfigureEntrySpot\s*\([\s\S]{0,160}?Action<", RegexOptions.Compiled),
                "entry spot options callback"
            ),
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

        foreach (var document in DotNetContractDocuments)
            Assert.Contains(document, matrix, StringComparison.Ordinal);
    }

    [Fact]
    public void DotNetContractRegressionTestReferences_Resolve_ToActiveTestMethods()
    {
        var activeTests = GetActiveTestMethods();
        var unresolved = new List<string>();

        var document = "regression-test-matrix.ko.md";
        var references = ExtractRegressionTestReferences(File.ReadAllText(ResolveDoc(document)))
            .ToArray();
        Assert.NotEmpty(references);

        foreach (var reference in references)
            if (reference.StartsWith("SCRIPT:", StringComparison.Ordinal))
            {
                var languageRoot = Path.GetFullPath(
                    Path.Combine(GetDotNetDocRoot(), "..", "..", "..", "languages", "dotnet")
                );
                if (!File.Exists(Path.Combine(languageRoot, reference[7..])))
                    unresolved.Add($"{document}: {reference}");
            }
            else if (!activeTests.Contains(reference))
                unresolved.Add($"{document}: {reference}");

        Assert.True(
            unresolved.Count == 0,
            $"Unresolved regression references:{Environment.NewLine}{string.Join(Environment.NewLine, unresolved.Order(StringComparer.Ordinal))}"
        );
    }

    [Fact]
    public void DotNetExactInterfaceDocuments_Have_An_Explicit_Regression_Owner()
    {
        var interfaceRoot = Path.Combine(GetDotNetContractDocRoot(), "interfaces");
        var matrix = File.ReadAllText(ResolveDoc("regression-test-matrix.ko.md"));
        const string owner =
            "ContractSurfaceCoverage.DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports";

        var missing = Directory
            .EnumerateFiles(interfaceRoot, "*.ko.md", SearchOption.TopDirectoryOnly)
            .Where(path =>
                !string.Equals(Path.GetFileName(path), "README.ko.md", StringComparison.Ordinal)
            )
            .Where(path =>
            {
                var document = Path.GetFileName(path);
                return !Regex.IsMatch(
                    matrix,
                    $@"(?m)^\|\s*`{Regex.Escape(document)}`\s*\|.*`{Regex.Escape(owner)}`",
                    RegexOptions.CultureInvariant
                );
            })
            .Select(Path.GetFileName)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.True(
            missing.Length == 0,
            $"Exact interface documents without an explicit regression owner: {string.Join(", ", missing)}"
        );
    }

    [Fact]
    public void DotNetRegressionMatrix_Includes_ExecutionSerialization_Guards()
    {
        var matrix = File.ReadAllText(ResolveDoc("regression-test-matrix.ko.md"));

        Assert.Contains("Entry Spot actor mailbox dispatch", matrix, StringComparison.Ordinal);
        Assert.Contains("local actor mailbox dispatch", matrix, StringComparison.Ordinal);
        Assert.Contains("user Spot actor dispatch serialization", matrix, StringComparison.Ordinal);
        Assert.Contains("session actor dispatch ordering", matrix, StringComparison.Ordinal);
        Assert.Contains(
            "actor dispatch location after mailbox wait",
            matrix,
            StringComparison.Ordinal
        );
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
            StringComparison.Ordinal
        );
        Assert.Contains(
            "RunCpuWorker_Yield_Releases_And_Resumes_Through_Serial_Turn",
            matrix,
            StringComparison.Ordinal
        );
        Assert.Contains("E2E:ATD-B3", matrix, StringComparison.Ordinal);
        Assert.Contains("`Yield(...)`", matrix, StringComparison.Ordinal);
    }

    [Fact]
    public void SpotNodeContractsUseTheLocationStoreAsTheAddressSourceOfTruth()
    {
        // Location Store 요구 조건은 언어별 configuration exact interface가 소유한다.
        var dotnet = File.ReadAllText(
            Path.Combine(
                GetDotNetContractDocRoot(),
                "interfaces",
                "03-configuration-topology.ko.md"
            )
        );
        var node = File.ReadAllText(
            Path.GetFullPath(
                Path.Combine(GetDotNetContractDocRoot(), "..", "node", "01-system-structure.ko.md")
            )
        );

        Assert.Contains("location store", dotnet, StringComparison.Ordinal);
        Assert.Contains("location store", node, StringComparison.Ordinal);
        Assert.DoesNotContain("core `ResolveSpot", dotnet, StringComparison.Ordinal);
        Assert.DoesNotContain("core `resolveSpot", node, StringComparison.Ordinal);
    }

    [Fact]
    public void DotNetLanguageContractsPreserveTheReviewedPublicRuntimeDecisions()
    {
        var handlers = File.ReadAllText(
            Path.Combine(GetDotNetContractDocRoot(), "interfaces", "06-actors.ko.md")
        );
        var system = File.ReadAllText(
            Path.Combine(GetDotNetContractDocRoot(), "interfaces", "02-configuration-host.ko.md")
        );
        var routeMesh = File.ReadAllText(
            Path.Combine(
                GetDotNetContractDocRoot(),
                "interfaces",
                "03-configuration-topology.ko.md"
            )
        );
        var locationStore = File.ReadAllText(
            Path.Combine(GetDotNetContractDocRoot(), "interfaces", "08-authority-relocation.ko.md")
        );

        Assert.Contains(
            "public interface IZLinkMeshNodeBuilder",
            routeMesh,
            StringComparison.Ordinal
        );
        Assert.Contains(
            "public interface IZLinkMeshPeerConnections",
            routeMesh,
            StringComparison.Ordinal
        );
        Assert.Contains("string? packetName = null", routeMesh, StringComparison.Ordinal);
        Assert.Contains("`ZLinkConfigurationException`", system, StringComparison.Ordinal);
        Assert.Contains("ASP.NET Core", system, StringComparison.Ordinal);
        Assert.Contains("public interface IZLinkActorClient", handlers, StringComparison.Ordinal);
        Assert.Contains(
            "public interface IZLinkLocationStore",
            locationStore,
            StringComparison.Ordinal
        );
    }

    [Fact]
    public void CanonicalCommonSpecOwnsLiveDotNetContracts()
    {
        //  The canonical server chapters live under spec/server (the spec root
        //  also hosts http-client/stream-connector/draft).
        var specRoot = Path.Combine(GetCommonSpecRoot(), "server");
        var deletedSpecRoot = Path.GetFullPath(
            Path.Combine(GetCommonSpecRoot(), "..", "..", "spec")
        );
        //  주제 디렉터리로 재구성한 뒤의 경로다. 옛 전역 번호 문서는 archive/에 있다.
        var required = new[]
        {
            "02-channel-transport/01-channel-topology.ko.md",
            "03-spot-actor/04-actor-model.ko.md",
            "04-session/02-session-actor-binding.ko.md",
            "02-channel-transport/05-transport-liveness.ko.md",
        };
        var writingGuide = Path.GetFullPath(
            Path.Combine(
                GetCommonSpecRoot(),
                "..",
                "..",
                "..",
                "..",
                "..",
                "doc",
                "principal",
                "documentation",
                "spec-writing-guide.ko.md"
            )
        );

        Assert.False(Directory.Exists(deletedSpecRoot));
        Assert.All(required, file => Assert.True(File.Exists(Path.Combine(specRoot, file)), file));
        Assert.True(File.Exists(writingGuide), writingGuide);
        Assert.False(File.Exists(Path.Combine(specRoot, "00-spec-writing-guide.ko.md")));
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
                "dotnet"
            );

            if (Directory.Exists(candidate))
                return candidate;

            current = current.Parent;
        }

        throw new DirectoryNotFoundException(
            "Could not find framework/doc/framework/dotnet from test runtime."
        );
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
            "dotnet"
        );
        var httpClientRoot = Path.Combine(
            GetCommonSpecRoot(),
            "http-client",
            "languages",
            "dotnet"
        );

        return Directory
            .EnumerateFiles(GetDotNetContractDocRoot(), "*.ko.md", SearchOption.TopDirectoryOnly)
            .Concat(
                Directory.EnumerateFiles(connectorRoot, "*.ko.md", SearchOption.TopDirectoryOnly)
            )
            .Concat(
                Directory.EnumerateFiles(httpClientRoot, "*.ko.md", SearchOption.TopDirectoryOnly)
            )
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
        return Path.GetFullPath(
            Path.Combine(GetDotNetDocRoot(), "..", "common", "guide", "server")
        );
    }

    /// <summary>
    /// 금칙 심볼 검사가 훑는 산문 문서 전체다 — `.NET` 문서 트리와 공통 가이드 정본.
    /// </summary>
    private static string[] GetDotNetAndCommonGuideDocs()
    {
        return Directory
            .EnumerateFiles(GetDotNetDocRoot(), "*.ko.md", SearchOption.AllDirectories)
            .Concat(
                Directory.EnumerateFiles(
                    GetCommonGuideServerRoot(),
                    "*.ko.md",
                    SearchOption.AllDirectories
                )
            )
            .Where(static path =>
                !path.Contains(
                    $"{Path.DirectorySeparatorChar}draft{Path.DirectorySeparatorChar}",
                    StringComparison.Ordinal
                )
            )
            .Order(StringComparer.Ordinal)
            .ToArray();
    }

    private static string GetDotNetContractDocRoot()
    {
        var dotnetDocRoot = GetDotNetDocRoot();
        var contractRoot = Path.GetFullPath(
            Path.Combine(dotnetDocRoot, "..", "common", "spec", "server", "languages", "dotnet")
        );

        return Directory.Exists(contractRoot)
            ? contractRoot
            : throw new DirectoryNotFoundException(
                "Could not find framework/doc/framework/common/spec/server/languages/dotnet from test runtime."
            );
    }

    private static string ResolveDoc(string fileName)
    {
        var matches = Directory
            .EnumerateFiles(GetDotNetDocRoot(), fileName, SearchOption.AllDirectories)
            .Concat(
                Directory.EnumerateFiles(
                    GetDotNetContractDocRoot(),
                    fileName,
                    SearchOption.TopDirectoryOnly
                )
            )
            .Where(path =>
                !string.Equals(Path.GetFileName(path), "README.ko.md", StringComparison.Ordinal)
                || string.Equals(
                    Path.GetDirectoryName(path),
                    GetDotNetDocRoot(),
                    StringComparison.Ordinal
                )
            )
            .ToArray();

        return matches.Length switch
        {
            1 => matches[0],
            0 => throw new FileNotFoundException($"Could not find .NET document '{fileName}'."),
            _ => throw new InvalidOperationException(
                $"Ambiguous document '{fileName}': {string.Join(", ", matches)}"
            ),
        };
    }

    private static IReadOnlyCollection<string> ExtractRegressionTestReferences(string text)
    {
        var sectionMatch = Regex.Match(
            text,
            @"^## [^\r\n]*회귀 테스트[^\r\n]*\r?\n(?<body>.*?)(?=^## |\z)",
            RegexOptions.Multiline | RegexOptions.Singleline
        );

        Assert.True(sectionMatch.Success, "Missing regression test section.");

        return Regex
            .Matches(
                sectionMatch.Groups["body"].Value,
                @"^\| (?<cell>.*?) \|",
                RegexOptions.Multiline
            )
            .SelectMany(static row =>
                Regex
                    .Matches(row.Groups["cell"].Value, @"`(?<test>[^`]+)`")
                    .Select(static match => match.Groups["test"].Value)
                    .Where(static value =>
                        !value.EndsWith(".ko.md", StringComparison.OrdinalIgnoreCase)
                    )
            )
            .ToArray();
    }

    private static IReadOnlySet<string> GetActiveTestMethods()
    {
        var testsRoot = GetTestsRoot();
        var sourceFiles = new HashSet<string>(StringComparer.Ordinal);

        foreach (
            var projectPath in EnumerateFilesSkippingGeneratedDirectories(testsRoot, "*.csproj")
        )
            AddProjectSources(projectPath, sourceFiles);

        var activeTests = new HashSet<string>(StringComparer.Ordinal);
        foreach (var sourceFile in sourceFiles)
        {
            var text = File.ReadAllText(sourceFile);
            var classMatches = Regex.Matches(
                text,
                @"(?m)^\s*(?:(?:public|internal|file|private|protected)\s+)*(?:(?:sealed|static|abstract|partial|new)\s+)*class\s+(?<class>[A-Za-z_][A-Za-z0-9_]*)\b"
            );

            if (classMatches.Count == 0)
                continue;

            foreach (
                Match methodMatch in Regex.Matches(
                    text,
                    @"\bpublic\s+(?:async\s+)?(?:Task|void)\s+(?<method>[A-Za-z_][A-Za-z0-9_]*)\s*\("
                )
            )
            {
                var className = classMatches
                    .Last(match => match.Index < methodMatch.Index)
                    .Groups["class"]
                    .Value;
                var methodName = methodMatch.Groups["method"].Value;
                if (HasFactOrTheoryAttribute(text, methodMatch.Index))
                    activeTests.Add($"{className}.{methodName}");
            }
        }

        return activeTests;
    }

    private static IEnumerable<string> ExtractLedgerProofReferences(string ledger)
    {
        foreach (var line in ledger.Split('\n'))
        {
            if (!line.StartsWith("| DN-", StringComparison.Ordinal))
                continue;
            var cells = line.Split('|');
            if (cells.Length < 6)
                continue;
            var proofCell = cells[^2];
            foreach (Match match in Regex.Matches(proofCell, @"`(?<reference>[^`]+)`"))
            {
                var reference = match.Groups["reference"].Value;
                if (
                    reference.StartsWith("E2E:", StringComparison.Ordinal)
                    || Regex.IsMatch(reference, @"^[A-Za-z_][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*$")
                )
                    yield return reference;
            }
        }
    }

    private static bool HasFactOrTheoryAttribute(string text, int methodIndex)
    {
        var prefix = text[..methodIndex];
        var factIndex = new[] { "[Fact", "[Theory", "[SkippableFact", "[SkippableTheory" }.Max(
            attribute => prefix.LastIndexOf(attribute, StringComparison.Ordinal)
        );
        if (factIndex < 0)
            return false;

        var priorMethod = Regex
            .Matches(prefix, @"\bpublic\s+(?:async\s+)?(?:Task|void)\s+[A-Za-z_][A-Za-z0-9_]*\s*\(")
            .Cast<Match>()
            .LastOrDefault();
        if (priorMethod is not null && factIndex < priorMethod.Index)
            return false;

        var attributes = prefix[factIndex..];
        return !Regex.IsMatch(attributes, @"\bSkip\s*=");
    }

    private static bool IsUnderDirectory(
        string path,
        string directory,
        bool includeDirectChildrenOnly
    )
    {
        var actualDirectory = Path.GetDirectoryName(path);
        if (actualDirectory is null)
            return false;

        if (includeDirectChildrenOnly)
            return string.Equals(actualDirectory, directory, StringComparison.Ordinal);

        var relative = Path.GetRelativePath(directory, actualDirectory);
        return relative == "."
            || (
                !relative.StartsWith("..", StringComparison.Ordinal) && !Path.IsPathRooted(relative)
            );
    }

    private static void AddProjectSources(string projectPath, ISet<string> sourceFiles)
    {
        var projectDirectory =
            Path.GetDirectoryName(projectPath)
            ?? throw new InvalidOperationException(
                $"Could not get project directory for '{projectPath}'."
            );
        var document = XDocument.Load(projectPath);
        var defaultCompileItems = document
            .Descendants("EnableDefaultCompileItems")
            .LastOrDefault()
            ?.Value;
        var projectSources = string.Equals(
            defaultCompileItems,
            "false",
            StringComparison.OrdinalIgnoreCase
        )
            ? new HashSet<string>(StringComparer.Ordinal)
            : Directory
                .EnumerateFiles(projectDirectory, "*.cs", SearchOption.AllDirectories)
                .Where(static path =>
                    !path.Contains(
                        $"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                        StringComparison.Ordinal
                    )
                    && !path.Contains(
                        $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                        StringComparison.Ordinal
                    )
                )
                .Select(Path.GetFullPath)
                .ToHashSet(StringComparer.Ordinal);

        foreach (
            var remove in document
                .Descendants("Compile")
                .SelectMany(static element => element.Attributes("Remove"))
        )
        foreach (var removedPath in ResolveProjectPattern(projectDirectory, remove.Value))
            projectSources.Remove(removedPath);

        foreach (
            var include in document
                .Descendants("Compile")
                .SelectMany(static element => element.Attributes("Include"))
        )
        foreach (var includedPath in ResolveProjectPattern(projectDirectory, include.Value))
            projectSources.Add(includedPath);

        foreach (var sourceFile in projectSources)
            sourceFiles.Add(sourceFile);
    }

    private static IEnumerable<string> ResolveProjectPattern(
        string projectDirectory,
        string pattern
    )
    {
        if (pattern.Contains('*', StringComparison.Ordinal))
            yield break;

        var path = Path.GetFullPath(Path.Combine(projectDirectory, pattern));
        if (File.Exists(path))
            yield return path;
    }

    private static string GetTestsRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);

        while (current is not null)
        {
            var candidate = Path.Combine(
                current.FullName,
                "framework",
                "languages",
                "dotnet",
                "tests"
            );
            if (Directory.Exists(candidate))
                return candidate;

            current = current.Parent;
        }

        throw new DirectoryNotFoundException(
            "Could not find framework/languages/dotnet/tests from test runtime."
        );
    }

    private static IEnumerable<string> EnumerateFilesSkippingGeneratedDirectories(
        string root,
        string pattern
    )
    {
        var pending = new Stack<string>();
        pending.Push(root);

        while (pending.Count > 0)
        {
            var directory = pending.Pop();

            foreach (
                var file in Directory.EnumerateFiles(
                    directory,
                    pattern,
                    SearchOption.TopDirectoryOnly
                )
            )
                yield return file;

            foreach (
                var child in Directory.EnumerateDirectories(
                    directory,
                    "*",
                    SearchOption.TopDirectoryOnly
                )
            )
            {
                var name = Path.GetFileName(child);
                if (name is "bin" or "obj" or "logs" or "nuget-packages" or "artifacts")
                    continue;
                pending.Push(child);
            }
        }
    }
}
