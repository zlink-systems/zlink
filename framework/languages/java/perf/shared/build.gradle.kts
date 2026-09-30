plugins {
    `java-library`
}

dependencies {
    api(zlinkLibs.zlink.framework.core)
    api(zlinkLibs.zlink.stream.connector)
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

// The §15.3 histogram bounds have one definition, framework/perf/schema; every language reads that file.
sourceSets.main {
    resources.srcDir("../../../../perf/schema")
}
