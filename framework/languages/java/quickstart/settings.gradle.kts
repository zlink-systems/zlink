pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

fun zlinkLocalMavenRepository(): java.io.File? {
    val configuredRoot =
        providers.gradleProperty("zlink.localPackageRoot")
            .orElse(providers.environmentVariable("ZLINK_LOCAL_PACKAGE_ROOT"))
            .orNull
    return configuredRoot?.takeIf { it.isNotBlank() }?.let { file(it).resolve("maven") }
}

dependencyResolutionManagement {
    repositories {
        zlinkLocalMavenRepository()?.let { localRepository ->
            maven { url = uri(localRepository) }
        }
        mavenCentral()
    }
}

rootProject.name = "zlink-quickstart"

// Gradle이 만드는 Windows 시작 스크립트는 jar를 하나씩 CLASSPATH에 적는다. 미러를 그대로 받은 경로에서
// 이 줄이 cmd.exe의 8191자 한도를 넘으면 "The input line is too long"으로 시작하지 못한다(Kotlin Client).
// `lib` 디렉터리 wildcard를 쓰면 jar 수와 무관하게 한 줄로 끝난다. tutorial/settings.gradle.kts와
// samples/gradle/zlink-jvm-baseline.settings.gradle.kts의 같은 블록과 짝이다(서로 독립으로 빌드되는 project다).
gradle.beforeProject {
    plugins.withType<ApplicationPlugin> {
        tasks.withType<CreateStartScripts>().configureEach {
            doLast {
                unixScript.writeText(
                    unixScript.readText().replace(Regex("""(?m)^CLASSPATH=.*$""")) {
                        "CLASSPATH=\$APP_HOME/lib/*"
                    },
                )
                windowsScript.writeText(
                    windowsScript.readText().replace(Regex("""(?m)^set CLASSPATH=.*$""")) {
                        "set CLASSPATH=%APP_HOME%\\lib\\*"
                    },
                )
            }
        }
    }
}

// Java quickstart.
include("java:Shared")
include("java:Server")
include("java:Client")

// Kotlin quickstart. Kotlin has no directory of its own in this repository —
// it lives under framework/languages/java, next to the Java sources.
include("kotlin:Shared")
include("kotlin:Server")
include("kotlin:Client")
