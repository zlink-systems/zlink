plugins {
    base
    idea
    id("org.jetbrains.kotlin.jvm") apply false
    id("org.jetbrains.kotlin.plugin.spring") apply false
}

idea {
    module {
        name = "zlink-framework-java-samples"
    }
}

// The settings file (filtered per language by scripts/tutorial/export_examples.py
// for the examples mirrors) is the only owner of the sample list: the aggregate
// tasks follow every included subproject that defines the task, so the Java-only
// and Kotlin-only mirrors build with the same task.
fun sampleTasks(name: String) = subprojects.map { sample -> sample.tasks.matching { it.name == name } }

tasks.register("buildAllSamples") {
    group = LifecycleBasePlugin.BUILD_GROUP
    description = "Builds every Java and Kotlin ZLink sample included in this IDE project."
    dependsOn(sampleTasks("build"))
}

tasks.register("cleanAllSamples") {
    group = LifecycleBasePlugin.BUILD_GROUP
    description = "Cleans every Java and Kotlin ZLink sample included in this IDE project."
    dependsOn(sampleTasks("clean"))
}

tasks.register("verifyPackageMode") {
    group = LifecycleBasePlugin.VERIFICATION_GROUP
    description = "Verifies that samples resolve published packages without composite source builds."
    doLast {
        check(gradle.extensions.extraProperties["zlink.samples.effectivePackageMode"] == true) {
            "Run outside the framework repository or use -Pzlink.samples.packageMode=true."
        }
        check(gradle.includedBuilds.isEmpty()) {
            "Package mode must not include framework or bindings source builds: " +
                gradle.includedBuilds.joinToString { it.name }
        }
        println("ZLINK_SAMPLE_INCLUDED_BUILD_COUNT=0")
    }
}
