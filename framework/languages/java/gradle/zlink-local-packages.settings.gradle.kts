fun zlinkFrameworkJavaRoot(): java.io.File {
    var current = settingsDir
    while (current.parentFile != null && !current.resolve("gradle/libs.versions.toml").isFile) {
        current = current.parentFile
    }
    return current
}

fun zlinkLocalMavenRepository(): java.io.File {
    val configuredRoot = providers.gradleProperty("zlink.localPackageRoot")
        .orElse(providers.environmentVariable("ZLINK_LOCAL_PACKAGE_ROOT"))
        .orNull
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
        wslRepo.isDirectory -> wslRepo
        windowsRepo.isDirectory -> windowsRepo
        else -> wslRepo
    }
}

val zlinkBindingsSource = providers.environmentVariable("ZLINK_JAVA_BINDINGS_SOURCE").orNull
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
    .orElse("https://maven.pkg.github.com/kairos-code-dev/zlink")
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
        maven {
            name = "zlinkLocalPackages"
            url = uri(zlinkLocalMavenRepository())
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
