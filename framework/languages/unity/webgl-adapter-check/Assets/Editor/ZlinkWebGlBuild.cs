// Batchmode WebGL builds of the adapter check, one per Code Optimization level.
//
//   Unity -batchmode -quit -nographics -buildTarget WebGL \
//     -executeMethod Zlink.Verification.Editor.ZlinkWebGlBuild.Build \
//     -zlinkOutput <dir> -zlinkOptimizations BuildTimes,RuntimeSpeed
//
// Both levels are built in one Editor session: the license activation, the
// project import and the IL2CPP toolchain warm-up are the expensive parts and
// they are paid once. A level that fails to build does not stop the others; the
// outcome of each lands in <dir>/build-summary.json, which the driver and the
// workflow read.
//
// There is deliberately no asmdef here. Assembly-CSharp-Editor already
// references the WebGL build module's UnityEditor.WebGL.Extensions assembly,
// which owns UserBuildSettings.codeOptimization, and that is the arrangement
// Unity's own "Use C# code to enable optimization settings" page documents.
//
// Nothing here references the runtime assembly. That one is WebGL-only, so the
// Editor cannot see its types - which is also why the player entry point is a
// [RuntimeInitializeOnLoadMethod] and the built scene is empty.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Zlink.Verification.Editor
{
    public static class ZlinkWebGlBuild
    {
        private const string ScenePath = "Assets/Generated/ZlinkWebGlAdapterCheck.unity";

        // The set this project exists to compare. -O1 is the level the adapter
        // README tells consumers to build at; -O2 is where emscripten runs its JS
        // optimizer over the .jspre plugins. Building only one of them answers
        // half the question, so both are the default and a caller has to say so
        // to get anything else.
        private const string DefaultOptimizations = "BuildTimes,RuntimeSpeed";

        public static void Build()
        {
            // Resolved against the Unity project folder, not the process working
            // directory: the container the build runs in decides the latter, and
            // a path that means something different in CI than locally is the
            // kind of thing that only shows up after a 30-minute build.
            var output = Resolve(Argument("-zlinkOutput") ?? "Builds");
            var levels = (Argument("-zlinkOptimizations") ?? DefaultOptimizations).Split(',');
            Console.WriteLine("ZLINK-BUILD levels " + string.Join(",", levels));

            Directory.CreateDirectory(output);
            var scene = CreateEmptyScene();
            ApplyFixedPlayerSettings();

            var results = new List<string>();
            var produced = 0;
            foreach (var raw in levels)
            {
                var level = raw.Trim();
                if (level.Length == 0) continue;

                var target = Path.Combine(output, level);
                var entry = BuildOne(scene, level, target);
                results.Add(entry.Json);
                if (entry.Succeeded) produced += 1;
            }

            var summary = "{\"unityVersion\":" + Quote(Application.unityVersion) +
                          ",\"builds\":[" + string.Join(",", results) + "]}";
            File.WriteAllText(Path.Combine(output, "build-summary.json"), summary);
            Console.WriteLine("ZLINK-BUILD summary " + summary);

            EditorApplication.Exit(produced == 0 ? 1 : 0);
        }

        private static Entry BuildOne(string scene, string level, string target)
        {
            object parsed;
            try
            {
                parsed = Enum.Parse(typeof(UnityEditor.WebGL.WasmCodeOptimization), level, false);
            }
            catch (Exception error)
            {
                // A level this Editor does not define is a fact about the Editor,
                // not a build failure to debug later.
                return Entry.Failed(level, "unknown WasmCodeOptimization: " + error.Message);
            }

            Console.WriteLine("ZLINK-BUILD " + level + " -> " + target);

            // Everything that can fail for one level is inside this try, so a level
            // that dies still leaves an entry in the summary and the next level
            // still runs. -O2 failing is an expected outcome here, not a reason to
            // lose the -O1 result.
            try
            {
                // The emscripten optimization level lives on the WebGL build
                // module, not on PlayerSettings: BuildTimes is -O1, RuntimeSpeed
                // and the rest are -O2 and above, which is where emscripten runs
                // its JS optimizer over the .jspre plugins.
                UnityEditor.WebGL.UserBuildSettings.codeOptimization =
                    (UnityEditor.WebGL.WasmCodeOptimization)parsed;

                if (Directory.Exists(target)) Directory.Delete(target, true);
                Directory.CreateDirectory(target);

                var report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
                {
                    scenes = new[] { scene },
                    locationPathName = target,
                    target = BuildTarget.WebGL,
                    targetGroup = BuildTargetGroup.WebGL,
                    options = BuildOptions.None
                });

                var summary = report.summary;
                if (summary.result != BuildResult.Succeeded)
                {
                    return Entry.Failed(level, summary.result + ", " + summary.totalErrors + " errors");
                }

                return new Entry(
                    level,
                    true,
                    null,
                    Path.GetFileName(target),
                    summary.totalTime.TotalSeconds,
                    summary.totalSize);
            }
            catch (Exception error)
            {
                return Entry.Failed(level, error.ToString());
            }
        }

        // An empty scene, because the player entry point is a
        // [RuntimeInitializeOnLoadMethod]: nothing in the scene is serialized, so
        // there is no authored asset to keep in step with the code.
        private static string CreateEmptyScene()
        {
            Directory.CreateDirectory(Path.GetDirectoryName(ScenePath));
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            if (!EditorSceneManager.SaveScene(scene, ScenePath))
            {
                throw new InvalidOperationException("could not save " + ScenePath);
            }

            AssetDatabase.Refresh();
            return ScenePath;
        }

        private static void ApplyFixedPlayerSettings()
        {
            PlayerSettings.companyName = "ZLink Systems";
            PlayerSettings.productName = "ZlinkWebGlAdapterCheck";

            // No Content-Encoding negotiation between the driver's static server
            // and the player, and the framework JavaScript stays readable in the
            // artifact so a link can be inspected after the fact.
            PlayerSettings.WebGL.compressionFormat = WebGLCompressionFormat.Disabled;
            PlayerSettings.WebGL.nameFilesAsHashes = false;
            PlayerSettings.WebGL.dataCaching = false;

            // Minimal is the WebGL default; setting it makes the level part of
            // this file rather than of whatever ProjectSettings.asset Unity
            // generates on first import. link.xml preserves the check's own entry
            // point and nothing else, so the adapter still faces real stripping.
            PlayerSettings.SetManagedStrippingLevel(NamedBuildTarget.WebGL, ManagedStrippingLevel.Minimal);
        }

        private static string Resolve(string relativeOrAbsolute)
        {
            if (Path.IsPathRooted(relativeOrAbsolute)) return relativeOrAbsolute;
            var projectRoot = System.IO.Directory.GetParent(Application.dataPath).FullName;
            return Path.GetFullPath(Path.Combine(projectRoot, relativeOrAbsolute));
        }

        /// <summary>
        ///     The value after <paramref name="name" />, or null when the flag is
        ///     absent. A flag that is present with no value throws: the caller meant
        ///     to pass something, and silently falling back to a default is how a
        ///     workflow that passed an empty expansion built one level instead of two.
        /// </summary>
        private static string Argument(string name)
        {
            var arguments = Environment.GetCommandLineArgs();
            for (var index = 0; index < arguments.Length; index += 1)
            {
                if (!string.Equals(arguments[index], name, StringComparison.Ordinal)) continue;

                var value = index + 1 < arguments.Length ? arguments[index + 1] : null;
                if (string.IsNullOrEmpty(value) || value.StartsWith("-", StringComparison.Ordinal))
                {
                    throw new ArgumentException(name + " was passed with no value");
                }

                return value;
            }

            return null;
        }

        private static string Quote(string value)
        {
            var text = new StringBuilder("\"");
            foreach (var character in value ?? string.Empty)
            {
                if (character == '"' || character == '\\') text.Append('\\').Append(character);
                else if (character == '\n') text.Append("\\n");
                else if (character == '\r') text.Append("\\r");
                else if (character < ' ') text.Append("\\u").Append(((int)character).ToString("x4"));
                else text.Append(character);
            }

            return text.Append('"').ToString();
        }

        private readonly struct Entry
        {
            public Entry(string level, bool succeeded, string reason, string directory, double seconds, ulong size)
            {
                Level = level;
                Succeeded = succeeded;
                Reason = reason;
                Directory = directory;
                Seconds = seconds;
                Size = size;
            }

            public bool Succeeded { get; }

            private string Level { get; }

            private string Reason { get; }

            private string Directory { get; }

            private double Seconds { get; }

            private ulong Size { get; }

            public string Json
            {
                get
                {
                    return "{\"level\":" + Quote(Level) +
                           ",\"succeeded\":" + (Succeeded ? "true" : "false") +
                           ",\"directory\":" + (Directory == null ? "null" : Quote(Directory)) +
                           ",\"seconds\":" + Seconds.ToString("0.0", CultureInfo.InvariantCulture) +
                           ",\"bytes\":" + Size.ToString(CultureInfo.InvariantCulture) +
                           ",\"reason\":" + (Reason == null ? "null" : Quote(Reason)) + "}";
                }
            }

            public static Entry Failed(string level, string reason)
            {
                return new Entry(level, false, reason, null, 0, 0);
            }
        }
    }
}
