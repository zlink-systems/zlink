rootProject.name = "zlink-java-perf"

include(":perf-single")
include(":perf-multi")

project(":perf-single").projectDir = file("single/Zlink.BindingBench")
project(":perf-multi").projectDir = file("multi/Zlink.BindingBench.Multi")
