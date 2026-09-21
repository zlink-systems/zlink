import org.gradle.api.plugins.ApplicationPlugin
import org.gradle.api.plugins.JavaPlugin
import org.gradle.api.plugins.JavaPluginExtension
import org.gradle.jvm.application.tasks.CreateStartScripts
import org.gradle.jvm.toolchain.JavaLanguageVersion

val zlinkJavaLanguageVersion = 25

pluginManagement {
    plugins {
        id("org.jetbrains.kotlin.jvm") version "2.3.21"
        id("org.jetbrains.kotlin.plugin.spring") version "2.3.21"
    }
}

if (!gradle.extensions.extraProperties.has("zlink.jvmBaselineConfigured")) {
    gradle.extensions.extraProperties["zlink.jvmBaselineConfigured"] = true
    gradle.extensions.extraProperties["zlink.javaLanguageVersion"] = zlinkJavaLanguageVersion

    gradle.beforeProject {
        plugins.withType<JavaPlugin> {
            extensions.configure<JavaPluginExtension> {
                toolchain.languageVersion.set(
                    JavaLanguageVersion.of(zlinkJavaLanguageVersion),
                )
            }
        }

        // `installDist`가 만드는 `bin/<app>(.bat)`는 기본으로 모든 runtime jar를 한 줄에
        // 나열한다. 자연스러운 clone 깊이(examples 저장소를 받아 그대로 둔 경로)에서 `%APP_HOME%`
        // 치환 후 그 줄이 Windows cmd.exe의 8191자 한도를 넘으면
        // "입력 파일이 너무 깁니다"로 죽는다 - 이 샘플들은 70개 안팎의 jar를 물고 있어
        // 여기 걸린다. jar를 하나씩 적는 대신 `lib` 디렉터리 wildcard를 쓰면 jar 수와
        // 무관하게 한 줄로 끝난다.
        plugins.withType<ApplicationPlugin> {
            tasks.withType<CreateStartScripts>().configureEach {
                doLast {
                    // `String.replace(Regex, String)`의 치환 문자열은 `$`/`\`를 group
                    // 참조·escape로 읽는다("$APP_HOME"이 잘못된 group 참조로 예외를 낸다). 그
                    // 대신 lambda overload를 쓰면 반환값을 그대로(리터럴로) 쓴다.
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
}
