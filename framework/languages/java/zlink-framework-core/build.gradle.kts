import java.io.File

plugins {
    `java-library`
    `maven-publish`
}

description = "ZLink Framework Java core contracts, runtime, handler scanner, and backend adapter"

java {
    modularity.inferModulePath.set(true)
}

tasks.withType<JavaCompile>().configureEach {
    // Qualified exports name companion modules that intentionally depend on
    // this artifact, so they are absent while the core module itself compiles.
    options.compilerArgs.add("-Xlint:-module")
}

sourceSets {
    main {
        java.srcDir("../../../runtime/protocol/generated/jvm")
    }

    val m5Foundation by creating {
        java.setSrcDirs(listOf(
            "src/main/java",
            "../../../runtime/protocol/generated/jvm"
        ))
        java.include(
            "systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java",
            "systems/zlink/framework/runtime/internal/service/**",
            "ServiceWireConstants.java"
        )
        compileClasspath += configurations.compileClasspath.get()
        runtimeClasspath += output + compileClasspath + configurations.runtimeClasspath.get()
    }

    val m5FoundationTest by creating {
        java.setSrcDirs(listOf("src/test/java"))
        java.include(
            "systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePortContractTest.java",
            "systems/zlink/framework/runtime/internal/service/**"
        )
        compileClasspath += m5Foundation.output + configurations.testCompileClasspath.get()
        runtimeClasspath += output + compileClasspath + configurations.testRuntimeClasspath.get()
    }

    configurations.named(m5FoundationTest.implementationConfigurationName) {
        extendsFrom(configurations.testImplementation.get())
    }
    configurations.named(m5FoundationTest.runtimeOnlyConfigurationName) {
        extendsFrom(configurations.testRuntimeOnly.get())
    }

tasks.register<Test>("m5FoundationTest") {
        description = "Runs the isolated RouteMesh v11 JVM M5 foundation contract."
        group = LifecycleBasePlugin.VERIFICATION_GROUP
        testClassesDirs = m5FoundationTest.output.classesDirs
        classpath = m5FoundationTest.runtimeClasspath
        useJUnitPlatform()
    }
}

val verifyPublicContractModuleBoundary by tasks.registering {
    description =
        "Verifies that JPMS exports application contracts but hides runtime internals."
    group = LifecycleBasePlugin.VERIFICATION_GROUP
    val starterJar = project(":zlink-framework-spring-boot-starter")
        .tasks.named<Jar>("jar")
    val testkitJar = project(":zlink-framework-testkit")
        .tasks.named<Jar>("jar")
    dependsOn(tasks.jar, starterJar, testkitJar)

    doLast {
        val compiler = javax.tools.ToolProvider.getSystemJavaCompiler()
            ?: error("A JDK compiler is required")
        val companionProjects = listOf(
            project(":zlink-framework-spring-boot-starter"),
            project(":zlink-framework-testkit"),
        )
        val modulePath = buildList {
            add(tasks.jar.get().archiveFile.get().asFile.absolutePath)
            add(starterJar.get().archiveFile.get().asFile.absolutePath)
            add(testkitJar.get().archiveFile.get().asFile.absolutePath)
            addAll(configurations.runtimeClasspath.get().files.map { it.absolutePath })
            companionProjects.forEach { companion ->
                addAll(companion.configurations.getByName("runtimeClasspath").files
                    .map { it.absolutePath })
            }
        }.distinct().joinToString(File.pathSeparator)

        fun compileFixture(name: String): Int {
            val sourceRoot = layout.projectDirectory
                .dir("src/moduleBoundaryTest/$name").asFile
            val sources = sourceRoot.walkTopDown()
                .filter { it.isFile && it.extension == "java" }
                .map { it.absolutePath }
                .toList()
            val output = layout.buildDirectory
                .dir("module-boundary/$name").get().asFile
            output.deleteRecursively()
            output.mkdirs()
            return compiler.run(
                null,
                null,
                null,
                "--module-path",
                modulePath,
                "-d",
                output.absolutePath,
                *sources.toTypedArray(),
            )
        }

        check(compileFixture("positive") == 0) {
            "application contract package must remain exported"
        }
        check(compileFixture("negative") != 0) {
            "runtime/internal binding package must not be externally accessible"
        }
        check(compileFixture("directStartNegative") != 0) {
            "applications must not start ZLinkFrameworkRuntime directly"
        }
        check(compileFixture("companionInternalNegative") != 0) {
            "companion implementation packages must not be exported"
        }

        fun compileClasspathFixture(name: String): Int {
            val sourceRoot = layout.projectDirectory
                .dir("src/classpathBoundaryTest/$name").asFile
            val sources = sourceRoot.walkTopDown()
                .filter { it.isFile && it.extension == "java" }
                .map { it.absolutePath }
                .toList()
            val output = layout.buildDirectory
                .dir("classpath-boundary/$name").get().asFile
            output.deleteRecursively()
            output.mkdirs()
            val publicClasspath = buildList {
                add(tasks.jar.get().archiveFile.get().asFile.absolutePath)
                addAll(configurations.compileClasspath.get().files
                    .filterNot { it.name.startsWith("zlink-framework-binding-internal") }
                    .map { it.absolutePath })
            }.distinct().joinToString(File.pathSeparator)
            return compiler.run(
                null,
                null,
                null,
                "-classpath",
                publicClasspath,
                "-d",
                output.absolutePath,
                *sources.toTypedArray(),
            )
        }

        check(compileClasspathFixture("positive") == 0) {
            "application contract must compile on the public classpath"
        }
        check(compileClasspathFixture("negative") != 0) {
            "raw binding declarations must be absent from the public classpath"
        }
        check(compileClasspathFixture("directStartNegative") != 0) {
            "classpath applications must not start ZLinkFrameworkRuntime directly"
        }

        val companionModulePath = buildList {
            add(tasks.jar.get().archiveFile.get().asFile)
            add(starterJar.get().archiveFile.get().asFile)
            add(testkitJar.get().archiveFile.get().asFile)
            addAll(configurations.runtimeClasspath.get().files)
            companionProjects.forEach { companion ->
                addAll(companion.configurations.getByName("runtimeClasspath").files)
            }
        }.filter { it.isFile && it.extension == "jar" }
            .distinctBy { it.absolutePath }
            .joinToString(File.pathSeparator) { it.absolutePath }
        val javaExecutable = File(
            System.getProperty("java.home"),
            "bin/java",
        ).absolutePath
        val validation = ProcessBuilder(
            javaExecutable,
            "--validate-modules",
            "--module-path",
            companionModulePath,
        ).inheritIO().start()
        check(validation.waitFor() == 0) {
            "core, Spring starter, and testkit must resolve on one module path"
        }

        val smokeSourceRoot = layout.projectDirectory
            .dir("src/moduleStartupTest/spring").asFile
        val smokeSources = smokeSourceRoot.walkTopDown()
            .filter { it.isFile && it.extension == "java" }
            .map { it.absolutePath }
            .toList()
        val smokeOutput = layout.buildDirectory
            .dir("module-startup/spring").get().asFile
        smokeOutput.deleteRecursively()
        smokeOutput.mkdirs()
        check(compiler.run(
            null,
            null,
            null,
            "--module-path",
            companionModulePath,
            "--patch-module",
            "zlink.framework.spring.boot.starter=${smokeSourceRoot.absolutePath}",
            "-d",
            smokeOutput.absolutePath,
            *smokeSources.toTypedArray(),
        ) == 0) {
            "modular Spring bootstrap smoke must compile"
        }
        val smoke = ProcessBuilder(
            javaExecutable,
            "--module-path",
            companionModulePath,
            "--patch-module",
            "zlink.framework.spring.boot.starter=${smokeOutput.absolutePath}",
            "-m",
            "zlink.framework.spring.boot.starter/"
                + "systems.zlink.framework.spring.internal.runtime."
                + "ZLinkFrameworkBootstrapSmoke",
        ).inheritIO().start()
        check(smoke.waitFor() == 0) {
            "modular Spring bootstrap smoke must start the runtime"
        }
    }
}

tasks.named("check") {
    dependsOn(verifyPublicContractModuleBoundary)
}

dependencies {
    implementation(project(":zlink-framework-binding-internal"))
    api(project(":zlink-framework-provider-abstractions"))
    api(zlinkLibs.zlink.bindings)
    compileOnlyApi("org.jspecify:jspecify:1.0.0")
    api("io.netty:netty-buffer:4.1.100.Final")
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("org.lz4:lz4-java:1.8.0")
    implementation("org.slf4j:slf4j-api:2.0.16")
}
