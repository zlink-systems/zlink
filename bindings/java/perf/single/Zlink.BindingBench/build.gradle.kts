plugins {
    application
}

val perfBuildDir = providers.gradleProperty("zlinkPerfBuildDir").orNull
if (!perfBuildDir.isNullOrBlank()) {
    layout.buildDirectory.set(file(perfBuildDir))
}

repositories {
    mavenCentral()
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

sourceSets {
    named("main") {
        java.setSrcDirs(listOf("src/main/java", "../../common/src/main/java"))
    }
}

dependencies {
    implementation(project(":"))
    implementation("io.netty:netty-buffer:4.1.100.Final")
}

application {
    applicationName = "zlink-java-perf-single"
    mainClass.set("systems.zlink.perf.single.PerfMain")
    applicationDefaultJvmArgs = listOf(
        "--enable-native-access=ALL-UNNAMED",
        "-server",
        "-XX:TieredStopAtLevel=4",
    )
}

tasks.withType<JavaExec>().configureEach {
    jvmArgs("--enable-native-access=ALL-UNNAMED", "-server", "-XX:TieredStopAtLevel=4")
}

tasks.named<JavaExec>("run") {
    jvmArgs("--enable-native-access=ALL-UNNAMED", "-server", "-XX:TieredStopAtLevel=4")
}

tasks.register<JavaExec>("runMessageOutboundMicrobench") {
    group = "benchmark"
    description = "Run Java outbound message-path microbenchmarks"
    dependsOn("classes")
    classpath = sourceSets["main"].runtimeClasspath
    mainClass.set("systems.zlink.perf.MessageOutboundMicrobench")
    jvmArgs("--enable-native-access=ALL-UNNAMED", "-server", "-XX:TieredStopAtLevel=4")
}
