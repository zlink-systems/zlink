fun zlinkFrameworkJavaRoot(): java.io.File {
    var current = settingsDir
    while (current.parentFile != null && !current.resolve("gradle/libs.versions.toml").isFile) {
        current = current.parentFile
    }
    return current
}

apply(from = zlinkFrameworkJavaRoot()
    .resolve("samples/gradle/zlink-jvm-baseline.settings.gradle.kts"))

val zlinkIsWindows = System.getProperty("os.name").startsWith("Windows", ignoreCase = true)
val zlinkLocalRootFromEnvironment = providers.environmentVariable("ZLINK_LOCAL_PACKAGE_ROOT").orNull
val zlinkLocalRootFromProperty = providers.gradleProperty("zlink.localPackageRoot").orNull
val zlinkStrictFromEnvironment = providers.environmentVariable("ZLINK_JAVA_REQUIRE_LOCAL_BINDING").orNull
val zlinkStrictFromProperty = providers.gradleProperty("zlink.requireLocalBinding").orNull

fun zlinkStrictBoolean(value: String?, source: String): Boolean? {
    if (value.isNullOrBlank()) {
        return null
    }
    return when (value.lowercase()) {
        "true" -> true
        "false" -> false
        else -> error("$source must be 'true' or 'false'.")
    }
}

val zlinkEnvironmentRequiresLocalBinding =
    zlinkStrictBoolean(zlinkStrictFromEnvironment, "ZLINK_JAVA_REQUIRE_LOCAL_BINDING") == true

if (zlinkIsWindows && zlinkEnvironmentRequiresLocalBinding) {
    if (zlinkLocalRootFromEnvironment.isNullOrBlank()) {
        error("ZLINK_LOCAL_PACKAGE_ROOT is required by Windows strict local-binding mode.")
    }
    if (!zlinkLocalRootFromProperty.isNullOrBlank()
        && file(zlinkLocalRootFromProperty).canonicalFile
            != file(zlinkLocalRootFromEnvironment).canonicalFile) {
        error("zlink.localPackageRoot cannot override ZLINK_LOCAL_PACKAGE_ROOT in Windows strict local-binding mode.")
    }
    if (zlinkStrictBoolean(zlinkStrictFromProperty, "zlink.requireLocalBinding") == false) {
        error("zlink.requireLocalBinding cannot disable Windows strict local-binding mode.")
    }
}

val zlinkRequireLocalBinding = if (zlinkIsWindows && zlinkEnvironmentRequiresLocalBinding) {
    true
} else {
    zlinkStrictBoolean(zlinkStrictFromProperty, "zlink.requireLocalBinding")
        ?: zlinkStrictBoolean(zlinkStrictFromEnvironment, "ZLINK_JAVA_REQUIRE_LOCAL_BINDING")
        ?: false
}

fun zlinkLocalMavenRepository(): java.io.File {
    val configuredRoot = if (zlinkIsWindows && zlinkEnvironmentRequiresLocalBinding) {
        zlinkLocalRootFromEnvironment
    } else {
        zlinkLocalRootFromProperty ?: zlinkLocalRootFromEnvironment
    }
    if (!configuredRoot.isNullOrBlank()) {
        return file(configuredRoot).resolve("maven")
    }
    var current = settingsDir
    while (current.parentFile != null && !current.resolve(".git").exists()) {
        current = current.parentFile
    }
    val wslRepo = current.resolve(".artifacts/wsl/maven")
    val windowsRepo = current.resolve(".artifacts/windows/maven")
    return when {
        zlinkIsWindows -> windowsRepo
        wslRepo.isDirectory -> wslRepo
        windowsRepo.isDirectory -> windowsRepo
        else -> wslRepo
    }
}

val zlinkBindingsSource = providers.environmentVariable("ZLINK_JAVA_BINDINGS_SOURCE").orNull
if (zlinkRequireLocalBinding && !zlinkBindingsSource.isNullOrBlank()) {
    error("Strict local-package validation cannot use ZLINK_JAVA_BINDINGS_SOURCE.")
}
if (!zlinkBindingsSource.isNullOrBlank()) {
    includeBuild(file(zlinkBindingsSource)) {
        name = "zlink-bindings-java"
        dependencySubstitution {
            substitute(module("systems.zlink:zlink")).using(project(":"))
        }
    }
}

val zlinkGitHubPackagesUrl = providers.gradleProperty("zlink.githubPackagesUrl")
    .orElse(providers.environmentVariable("ZLINK_GITHUB_PACKAGES_URL"))
    .orElse("https://maven.pkg.github.com/zlink-systems/zlink")
val zlinkGitHubPackagesUser = providers.gradleProperty("zlink.githubPackagesUser")
    .orElse(providers.environmentVariable("MAVEN_REPOSITORY_USERNAME"))
    .orElse(providers.environmentVariable("GITHUB_ACTOR"))
val zlinkGitHubPackagesToken = providers.gradleProperty("zlink.githubPackagesToken")
    .orElse(providers.environmentVariable("MAVEN_REPOSITORY_PASSWORD"))
    .orElse(providers.environmentVariable("GITHUB_TOKEN"))

dependencyResolutionManagement {
    versionCatalogs {
        create("zlinkLibs") {
            from(files(zlinkFrameworkJavaRoot().resolve("gradle/libs.versions.toml")))
        }
    }
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        if (zlinkRequireLocalBinding) {
            exclusiveContent {
                forRepository {
                    maven {
                        name = "zlinkLocalPackages"
                        url = uri(zlinkLocalMavenRepository())
                    }
                }
                filter {
                    includeModule("systems.zlink", "zlink")
                }
            }
        } else {
            maven {
                name = "zlinkLocalPackages"
                url = uri(zlinkLocalMavenRepository())
            }
        }
        mavenCentral()
        val packageUser = zlinkGitHubPackagesUser.orNull
        val packageToken = zlinkGitHubPackagesToken.orNull
        if (!packageUser.isNullOrBlank() && !packageToken.isNullOrBlank()) {
            maven {
                name = "zlinkGitHubPackages"
                url = uri(zlinkGitHubPackagesUrl.get())
                credentials {
                    username = packageUser
                    password = packageToken
                }
            }
        }
    }
}
