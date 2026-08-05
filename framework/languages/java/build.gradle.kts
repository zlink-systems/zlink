plugins {
    idea
    id("org.jetbrains.kotlin.jvm") version "2.2.21" apply false
    kotlin("plugin.spring") version "2.2.21"
}

group = "systems.zlink"
version = "0.1.0-SNAPSHOT"

idea {
    module {
        name = "zlink-framework-java"
    }
}

val junitVersion = "5.10.2"
val localCoreRuntime = rootProject.file("../../../core/build/lib/libzlink.so")

subprojects {
    group = rootProject.group
    version = rootProject.version

    plugins.withId("org.jetbrains.kotlin.jvm") {
        val localBindingsBuild = gradle.includedBuilds.find { it.name == "zlink-bindings-java" }
        if (localBindingsBuild != null) {
            tasks.matching { it.name == "compileKotlin" || it.name == "compileTestKotlin" }.configureEach {
                dependsOn(localBindingsBuild.task(":jar"))
            }
        }
    }

    plugins.withType<JavaPlugin> {
        val localBindingsBuild = gradle.includedBuilds.find { it.name == "zlink-bindings-java" }
        if (localBindingsBuild != null) {
            tasks.withType<JavaCompile>().configureEach {
                dependsOn(localBindingsBuild.task(":jar"))
            }
        }

        extensions.configure<JavaPluginExtension> {
            toolchain {
                languageVersion.set(JavaLanguageVersion.of(22))
            }
        }

        extensions.configure<SourceSetContainer> {
            val main by getting
            val test by getting

            val contractTest by creating {
                java.srcDir("src/contractTest/java")
                resources.srcDir("src/contractTest/resources")
                compileClasspath += main.output + test.compileClasspath
                runtimeClasspath += output + compileClasspath + test.runtimeClasspath
            }

            val fakeBackendTest by creating {
                java.srcDir("src/fakeBackendTest/java")
                resources.srcDir("src/fakeBackendTest/resources")
                compileClasspath += main.output + test.compileClasspath
                runtimeClasspath += output + compileClasspath + test.runtimeClasspath
            }

            val integrationTest by creating {
                java.srcDir("src/integrationTest/java")
                resources.srcDir("src/integrationTest/resources")
                compileClasspath += main.output + test.compileClasspath
                runtimeClasspath += output + compileClasspath + test.runtimeClasspath
            }

            val sampleTest by creating {
                java.srcDir("src/sampleTest/java")
                resources.srcDir("src/sampleTest/resources")
                compileClasspath += main.output + test.compileClasspath
                runtimeClasspath += output + compileClasspath + test.runtimeClasspath
            }
        }

        configurations.named("contractTestImplementation") {
            extendsFrom(configurations.getByName("testImplementation"))
        }
        configurations.named("contractTestRuntimeOnly") {
            extendsFrom(configurations.getByName("testRuntimeOnly"))
        }
        configurations.named("fakeBackendTestImplementation") {
            extendsFrom(configurations.getByName("testImplementation"))
        }
        configurations.named("fakeBackendTestRuntimeOnly") {
            extendsFrom(configurations.getByName("testRuntimeOnly"))
        }
        configurations.named("integrationTestImplementation") {
            extendsFrom(configurations.getByName("testImplementation"))
        }
        configurations.named("integrationTestRuntimeOnly") {
            extendsFrom(configurations.getByName("testRuntimeOnly"))
        }
        configurations.named("sampleTestImplementation") {
            extendsFrom(configurations.getByName("testImplementation"))
        }
        configurations.named("sampleTestRuntimeOnly") {
            extendsFrom(configurations.getByName("testRuntimeOnly"))
        }

        dependencies {
            add("testImplementation", platform("org.junit:junit-bom:$junitVersion"))
            add("testImplementation", "org.junit.jupiter:junit-jupiter")
            add("testRuntimeOnly", "org.junit.platform:junit-platform-launcher")
        }

        tasks.withType<Test>().configureEach {
            useJUnitPlatform()
            jvmArgs("--enable-native-access=ALL-UNNAMED")
            if (localCoreRuntime.isFile) {
                environment("ZLINK_LIBRARY_PATH", localCoreRuntime.absolutePath)
            }
        }

        fun registerSourceSetTestTask(taskName: String, sourceSetName: String) {
            val sourceSets = extensions.getByType<SourceSetContainer>()
            val sourceSet = sourceSets.getByName(sourceSetName)
            tasks.register<Test>(taskName) {
                description = "Runs the $sourceSetName source set."
                group = LifecycleBasePlugin.VERIFICATION_GROUP
                testClassesDirs = sourceSet.output.classesDirs
                classpath = sourceSet.runtimeClasspath
                shouldRunAfter(tasks.named("test"))
                useJUnitPlatform()
                if (taskName == "integrationTest") {
                    forkEvery = 1
                    maxParallelForks = 1
                }
            }
            tasks.named("check") {
                dependsOn(taskName)
            }
        }

        registerSourceSetTestTask("contractTest", "contractTest")
        registerSourceSetTestTask("fakeBackendTest", "fakeBackendTest")
        registerSourceSetTestTask("integrationTest", "integrationTest")
        registerSourceSetTestTask("sampleTest", "sampleTest")
    }

    plugins.withId("maven-publish") {
        extensions.configure<PublishingExtension> {
            publications {
                create<MavenPublication>("mavenJava") {
                    from(components["java"])
                    groupId = project.group.toString()
                    artifactId = project.name
                    version = project.version.toString()
                    pom {
                        name.set(project.name)
                        description.set(project.description ?: project.name)
                        url.set("https://github.com/kairos-code-dev/zlink")
                        licenses {
                            // http-client wraps java.net.http (JDK standard library)
                            // rather than any zlink-original transport, so it ships
                            // under Apache-2.0 instead of the framework-wide FSL.
                            if (project.name in setOf("zlink-http-client", "zlink-http-client-kotlin")) {
                                license {
                                    name.set("Apache-2.0")
                                    url.set("https://www.apache.org/licenses/LICENSE-2.0")
                                }
                            } else {
                                license {
                                    name.set("FSL-1.1-ALv2")
                                    url.set("https://github.com/kairos-code-dev/zlink/blob/main/framework/LICENSE")
                                }
                            }
                        }
                        scm {
                            connection.set("scm:git:https://github.com/kairos-code-dev/zlink.git")
                            developerConnection.set("scm:git:git@github.com:kairos-code-dev/zlink.git")
                            url.set("https://github.com/kairos-code-dev/zlink")
                        }
                    }
                }
            }
            repositories {
                maven {
                    name = "releaseRepo"
                    val configuredUrl = providers.environmentVariable("MAVEN_REPOSITORY_URL")
                    val githubRepo = providers.environmentVariable("GITHUB_REPOSITORY")
                    val repoUrl = configuredUrl.orElse(
                        githubRepo.map { "https://maven.pkg.github.com/$it" }
                    ).orElse(layout.buildDirectory.dir("repo").map { it.asFile.toURI().toString() })
                    url = uri(repoUrl.get())
                    // file:// local repositories reject any authentication scheme; only
                    // remote repositories take credentials.
                    if (!repoUrl.get().startsWith("file:")) {
                        credentials {
                            val configuredUser = providers.environmentVariable("MAVEN_REPOSITORY_USERNAME")
                            val configuredPassword = providers.environmentVariable("MAVEN_REPOSITORY_PASSWORD")
                            val githubActor = providers.environmentVariable("GITHUB_ACTOR")
                            val githubToken = providers.environmentVariable("GITHUB_TOKEN")
                            username = configuredUser.orElse(githubActor).orNull
                            password = configuredPassword.orElse(githubToken).orNull
                        }
                    }
                }
            }
        }
    }
}
