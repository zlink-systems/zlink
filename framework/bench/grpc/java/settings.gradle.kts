pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

val frameworkJavaRoot = file("../../../languages/java")

fun zlinkLocalMavenRepository(): java.io.File {
    val configuredRoot = providers.gradleProperty("zlink.localPackageRoot")
        .orElse(providers.environmentVariable("ZLINK_LOCAL_PACKAGE_ROOT"))
        .orNull
    if (!configuredRoot.isNullOrBlank()) {
        return file(configuredRoot).resolve("maven")
    }
    val repoRoot = file("../../../..")
    val wslRepo = repoRoot.resolve(".artifacts/wsl/maven")
    val windowsRepo = repoRoot.resolve(".artifacts/windows/maven")
    return if (wslRepo.isDirectory) wslRepo else windowsRepo
}

dependencyResolutionManagement {
    versionCatalogs {
        create("zlinkLibs") {
            from(files(frameworkJavaRoot.resolve("gradle/libs.versions.toml")))
        }
    }
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        maven {
            name = "zlinkLocalPackages"
            url = uri(zlinkLocalMavenRepository())
        }
        mavenCentral()
    }
}

rootProject.name = "zlink-java-bench-with-grpc"

if (gradle.parent == null) {
    includeBuild(frameworkJavaRoot) {
        name = "zlink-framework-java-build"
    }
}

include(":shared")
include(":grpc-server")
include(":zlink-raw-server")
include(":zlink-framework-server")
include(":client")
include(":kotlin-client")
include(":repro")
