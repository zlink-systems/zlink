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
        languageVersion.set(JavaLanguageVersion.of(libs.versions.java.get().toInt()))
    }
}

sourceSets {
    named("main") {
        java.setSrcDirs(listOf("src/main/java", "../../common/src/main/java"))
    }
}

dependencies {
    implementation(project(":"))
    implementation(libs.netty.buffer)
    testImplementation("org.junit.jupiter:junit-jupiter:5.10.2")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

tasks.test {
    useJUnitPlatform()
    jvmArgs("--enable-native-access=ALL-UNNAMED", "-server", "-XX:TieredStopAtLevel=4", "-Xms512m", "-Xmx2g")
}

application {
    applicationName = "zlink-java-perf-multi"
    mainClass.set("systems.zlink.perf.multi.PerfMain")
    applicationDefaultJvmArgs = listOf(
        "--enable-native-access=ALL-UNNAMED",
        "-server",
        "-XX:TieredStopAtLevel=4",
    )
}

tasks.withType<JavaExec>().configureEach {
    jvmArgs("--enable-native-access=ALL-UNNAMED", "-server", "-XX:TieredStopAtLevel=4", "-Xms512m", "-Xmx2g")
}

tasks.named<JavaExec>("run") {
    jvmArgs("--enable-native-access=ALL-UNNAMED", "-server", "-XX:TieredStopAtLevel=4", "-Xms512m", "-Xmx2g")
}
