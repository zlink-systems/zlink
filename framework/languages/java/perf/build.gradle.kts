// Common settings of every perf project: warnings are errors (as in the .NET perf), and each role process is an
// `application` whose installDist start script is what the common runner launches (framework/perf/runner/launchers.py).
plugins {
    base
}

subprojects {
    plugins.withType<JavaPlugin> {
        tasks.withType<JavaCompile>().configureEach {
            options.encoding = "UTF-8"
            options.compilerArgs.addAll(listOf("-Xlint:all,-serial,-processing,-requires-automatic,-classfile", "-Werror"))
        }
        dependencies {
            "testImplementation"("org.junit.jupiter:junit-jupiter:5.11.4")
            "testRuntimeOnly"("org.junit.platform:junit-platform-launcher:1.11.4")
        }
        tasks.withType<Test>().configureEach {
            useJUnitPlatform()
        }
    }
    plugins.withType<ApplicationPlugin> {
        extensions.configure<JavaApplication> {
            // The Java binding calls native code through the foreign function API.
            applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
        }
    }
}
