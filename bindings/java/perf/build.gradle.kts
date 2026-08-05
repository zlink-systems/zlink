plugins {
    java
}

val nativeOs = System.getProperty("os.name").lowercase()
val nativeArch = System.getProperty("os.arch").lowercase()
val nativeBridgeSupported = nativeOs.contains("linux") &&
    nativeArch in setOf("amd64", "x86_64")
val javaBindingsDir = projectDir.parentFile
val repoRoot = javaBindingsDir.parentFile.parentFile
val configuredCoreBuildDir = System.getenv("ZLINK_CORE_BUILD_DIR")
val coreBuildDir = if (!configuredCoreBuildDir.isNullOrBlank()) {
    file(configuredCoreBuildDir).let { if (it.isAbsolute) it else repoRoot.resolve(it.path) }
} else {
    repoRoot.resolve("core/build")
}
val generatedNativeResourcesDir = layout.buildDirectory.dir(
    "generated/zlink-native-resources/main",
)
val bridgeOutputDir = layout.buildDirectory.dir(
    "generated/zlink-native-resources/main/native/linux-x86_64",
)

repositories {
    mavenCentral()
}

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(22)
    }
}

sourceSets {
    named("main") {
        java.setSrcDirs(listOf("../src/main/java"))
        resources.setSrcDirs(listOf("../src/main/resources"))
        resources.srcDir(generatedNativeResourcesDir)
    }
}

dependencies {
    compileOnly("io.netty:netty-buffer:4.1.100.Final")
    runtimeOnly("io.netty:netty-buffer:4.1.100.Final")
}

tasks.register<Exec>("buildZlinkJavaBridge") {
    onlyIf { nativeBridgeSupported }
    val source = javaBindingsDir.resolve("native/src/zlink_java_reqrep_bridge.c")
    val outputDir = bridgeOutputDir.get().asFile
    val output = outputDir.resolve("libzlink_java_bridge.so")
    val linkDir = coreBuildDir.resolve("lib")
    val javaHome = file(System.getProperty("java.home"))
    val jniIncludeDir = javaHome.resolve("include")
    val jniOsIncludeDir = jniIncludeDir.resolve("linux")

    inputs.file(source)
    inputs.dir(repoRoot.resolve("core/include"))
    inputs.dir(repoRoot.resolve("core/src"))
    inputs.dir(repoRoot.resolve("core/external/boost"))
    inputs.files(fileTree(coreBuildDir) {
        include("*.h")
        include("**/*.h")
        include("lib/libzlink.so")
        include("lib/libzlink.so.*")
        include("libzlink.pc")
        exclude("**/tests/**")
    })
    inputs.dir(linkDir)
    inputs.dir(jniIncludeDir)
    inputs.dir(jniOsIncludeDir)
    outputs.file(output)
    outputs.upToDateWhen { false }

    doFirst {
        outputDir.mkdirs()
    }
    commandLine(
        "c++",
        "-shared",
        "-fPIC",
        "-std=c++17",
        "-O2",
        "-pthread",
        "-I", repoRoot.resolve("core/include").absolutePath,
        "-I", repoRoot.resolve("core/src").absolutePath,
        "-I", repoRoot.resolve("core/external/boost").absolutePath,
        "-I", coreBuildDir.absolutePath,
        "-I", jniIncludeDir.absolutePath,
        "-I", jniOsIncludeDir.absolutePath,
        "-L", linkDir.absolutePath,
        "-Wl,-rpath,\$ORIGIN",
        "-Wl,--no-as-needed",
        "-o", output.absolutePath,
        source.absolutePath,
        "-lzlink",
    )
}

tasks.named<ProcessResources>("processResources") {
    dependsOn(tasks.named("buildZlinkJavaBridge"))
    duplicatesStrategy = DuplicatesStrategy.EXCLUDE
}
