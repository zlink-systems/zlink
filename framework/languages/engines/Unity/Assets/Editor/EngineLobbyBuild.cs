using System;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;

namespace EngineLobby.Editor
{
    public static class EngineLobbyBuild
    {
        public static void BuildWindows()
        {
            PlayerSettings.SetScriptingBackend(
                NamedBuildTarget.Standalone,
                ScriptingImplementation.Mono2x
            );
            Build("Build/Windows/EngineLobby.exe", BuildTarget.StandaloneWindows64);
        }

        public static void BuildWebGL()
        {
            Build("Build/WebGL", BuildTarget.WebGL);
        }

        private static void Build(string output, BuildTarget target)
        {
            var report = BuildPipeline.BuildPlayer(
                new[] { "Assets/Scenes/EngineLobby.unity" },
                output,
                target,
                BuildOptions.None
            );
            if (report.summary.result != BuildResult.Succeeded)
                throw new Exception($"Engine Lobby {target} build: {report.summary.result}");
        }
    }
}
