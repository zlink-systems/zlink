import org.jetbrains.kotlin.gradle.tasks.KotlinCompile

plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

val diagnosticStage = providers.gradleProperty("benchDiagnosticStage").orElse("core").get()
require(diagnosticStage in setOf("core", "codec")) {
    "benchDiagnosticStage must be core or codec"
}
val ownerDiagnosticJar = providers.gradleProperty("zlinkBenchDiagnosticOwnerJar").orNull
    ?.let(::file)
    ?: error("zlinkBenchDiagnosticOwnerJar is required when :diagnostics is included")
require(ownerDiagnosticJar.isAbsolute && ownerDiagnosticJar.isFile) {
    "zlinkBenchDiagnosticOwnerJar must name an existing absolute diagnostic jar"
}

val generatedStageDir = layout.buildDirectory.dir("generated/sources/fixed-stage/main/java")
val writeFixedDiagnosticStage by tasks.registering {
    val output = generatedStageDir.map {
        it.file("systems/zlink/bench/withgrpc/diagnostics/FixedDiagnosticStage.java")
    }
    outputs.file(output)
    doLast {
        val file = output.get().asFile
        file.parentFile.mkdirs()
        file.writeText("""
            package systems.zlink.bench.withgrpc.diagnostics;

            /** Generated at compilation; process arguments never select a measurement stage. */
            public final class FixedDiagnosticStage {
                public static final String VALUE = "$diagnosticStage";
                private FixedDiagnosticStage() { }
            }
        """.trimIndent() + "\n")
    }
}

sourceSets.named("main") {
    java.srcDir(generatedStageDir)
}

tasks.withType<JavaCompile>().configureEach {
    dependsOn(writeFixedDiagnosticStage)
}
tasks.withType<KotlinCompile>().configureEach {
    dependsOn(writeFixedDiagnosticStage)
}

dependencies {
    implementation(files(ownerDiagnosticJar))
    implementation(project(":shared"))
    implementation(project(":client"))
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-codec-protobuf:0.10.0")
    implementation(zlinkLibs.zlink.bindings)
}

application {
    applicationName = "bench-zlink-staged-jvm"
    mainClass.set("systems.zlink.bench.withgrpc.diagnostics.StagedJavaBench")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
