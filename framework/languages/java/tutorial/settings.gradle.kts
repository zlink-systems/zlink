import org.gradle.api.plugins.ApplicationPlugin
import org.gradle.jvm.application.tasks.CreateStartScripts

pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

fun zlinkLocalMavenRepository(): java.io.File? {
    val configuredRoot = providers.gradleProperty("zlink.localPackageRoot")
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

rootProject.name = "zlink-tutorial"

// `installDist`가 만드는 `bin/<app>(.bat)`는 기본으로 모든 runtime jar를 한 줄에 나열한다.
// 자연스러운 압축 해제 깊이(zip을 받아 그대로 푼 경로, 예:
// `zlink-tutorial-java\kotlin\Server\build\install\Server\bin\Server.bat`)에서 `%APP_HOME%`
// 치환 후 그 줄이 Windows cmd.exe의 8191자 한도를 넘으면 "입력 파일이 너무 깁니다"로 죽는다 -
// Kotlin Server는 70개 안팎의 jar를 물어 여기 걸리고, Java Server(~55개)도 여유가 거의 없다.
// jar를 하나씩 적는 대신 `lib` 디렉터리 wildcard를 쓰면 jar 수와 무관하게 한 줄로 끝난다.
// (`framework/languages/java/samples/gradle/zlink-jvm-baseline.settings.gradle.kts`의 같은
// 블록과 짝이다 - samples와 tutorial은 별도 zip으로 묶이므로 파일을 공유하지 못한다.)
gradle.beforeProject {
    plugins.withType<ApplicationPlugin> {
        tasks.withType<CreateStartScripts>().configureEach {
            doLast {
                // `String.replace(Regex, String)`의 치환 문자열은 `$`/`\`를 group 참조·escape로
                // 읽는다("$APP_HOME"이 잘못된 group 참조로 예외를 낸다). lambda overload는
                // 반환값을 그대로(리터럴로) 쓴다.
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

// Java tutorial.
include("java:Shared")
include("java:Server")
include("java:Client")
include("java:StreamClient")
include("java:HttpClient")

// Kotlin has no directory of its own in this repository -- it lives under
// framework/languages/java, next to the Java sources. The same layout the
// quickstart uses.
include("kotlin:Shared")
include("kotlin:Server")
include("kotlin:Client")
include("kotlin:StreamClient")
include("kotlin:HttpClient")
