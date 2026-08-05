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
    testImplementation("org.junit.jupiter:junit-jupiter:5.10.2")
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
