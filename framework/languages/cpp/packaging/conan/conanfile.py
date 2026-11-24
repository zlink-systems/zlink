"""Conan recipe for the zlink C++ framework (HTTP client and stream connector)."""

import json
import os
from pathlib import Path

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd, cross_building
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, save

required_conan_version = ">=2.1"

# Conan and vcpkg use the protobuf 6.33 series with gRPC 1.84.0 (#4077).
# vcpkg uses protobuf 6.33.4, the nearest version in the same series.
ZLINK_FRAMEWORK_CPP_PROTOBUF_REQUIREMENT = "protobuf/6.33.5"

# This is the authoritative third-party name/version list for the framework.
# package-prebuilt.sh imports it with the options below, so its CMakeDeps graph
# matches the package graph without downloading zlink-cpp (the prebuilt build
# stages that first-party archive). vcpkg.json remains the explicit
# vcpkg fallback manifest: its port feature names are vcpkg-specific.
ZLINK_FRAMEWORK_CPP_THIRD_PARTY_REQUIREMENTS = (
    # CMakeLists.txt:22 unconditionally needs the api component.
    "opentelemetry-cpp/1.26.0",
    "boost/1.85.0",
    "nlohmann_json/3.11.3",
    "yyjson/0.12.0",
    "openssl/[>=3.0 <4]",
    "lz4/1.9.4",
    ZLINK_FRAMEWORK_CPP_PROTOBUF_REQUIREMENT,
    "abseil/20260107.1",
    # 1.3.15 builds its async client with Conan's libuv, but its requirement does
    # not propagate libuv headers to consumers of async_redis.h. Require the same
    # compatible range directly so CMakeDeps exposes those headers to Framework.
    "redis-plus-plus/1.3.15",
    "hiredis/1.3.0",
    "libuv/[>=1 <2]",
)


def generate_framework_cmake(conanfile, toolchain):
    dependencies = conanfile.dependencies
    protobuf = (dependencies.build if cross_building(conanfile) else dependencies.host)["protobuf"]
    protoc_name = "protoc.exe" if conanfile.settings_build.os == "Windows" else "protoc"
    protoc = Path(protobuf.package_folder, protobuf.cpp_info.bindirs[0], protoc_name)
    if not protoc.is_file():
        raise ConanInvalidConfiguration(f"Missing protoc in {protobuf.ref}: {protoc}")
    provider = Path(conanfile.generators_folder, "zlink-conan-protobuf.json")
    save(conanfile, str(provider), json.dumps({
        "protoc": protoc.resolve().as_posix(),
        "version": str(protobuf.ref.version),
    }))
    toolchain.variables["ZLINK_FRAMEWORK_CPP_CONAN_PROTOBUF"] = provider.as_posix()
    toolchain.generate()
    CMakeDeps(conanfile).generate()


class ZlinkFrameworkConan(ConanFile):
    name = "zlink-framework"
    version = "0.34.0"
    package_type = "static-library"
    license = "Elastic-2.0"
    homepage = "https://github.com/zlink-systems/zlink"
    description = "C++ framework, HTTP client and stream connector for zlink"
    settings = "os", "arch", "compiler", "build_type"
    options = {"fPIC": [True, False], "with_etcd": [True, False]}
    default_options = {
        "fPIC": True,
        "with_etcd": False,
        "boost/*:header_only": True,
        # Mirrors vcpkg.json's redis-plus-plus async-std feature.
        "redis-plus-plus/*:build_async": True,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def configure(self):
        if self.options.with_etcd:
            self.options["grpc/*"].secure = True
            for plugin in ("csharp", "node", "objective_c", "php", "python", "ruby"):
                setattr(self.options["grpc/*"], plugin + "_plugin", False)

    def requirements(self):
        self.requires("zlink-cpp/1.26.0", transitive_headers=True,
                      transitive_libs=True)
        for dependency in ZLINK_FRAMEWORK_CPP_THIRD_PARTY_REQUIREMENTS:
            self.requires(dependency, transitive_headers=True, transitive_libs=True)
        if self.options.with_etcd:
            self.requires("grpc/1.84.0", transitive_headers=False, transitive_libs=True)

    def build_requirements(self):
        if cross_building(self):
            self.tool_requires(ZLINK_FRAMEWORK_CPP_PROTOBUF_REQUIREMENT)

    def validate(self):
        if self.settings.compiler.get_safe("cppstd"):
            check_min_cppstd(self, "20")

    def source(self):
        # The archive wraps the prepared tree in one top-level directory.
        get(self, **self.conan_data["sources"][self.version], strip_root=True)
        for relative_path in (
            "CMakeLists.txt",
            "runtime/protocol/generated/cpp/service_wire_constants.hpp",
            "runtime/protocol/generated/cpp/service_wire_pilot_codec.hpp",
            "LICENSE",
        ):
            if not os.path.isfile(os.path.join(self.source_folder, relative_path)):
                raise ConanInvalidConfiguration(
                    "Incomplete framework C++ archive: missing " + relative_path
                    + ". Publish the output of scripts/package-source.py "
                    "and update conandata.yml."
                )

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["CMAKE_INSTALL_LIBDIR"] = "lib"
        tc.variables["ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST"] = True
        tc.variables["ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK"] = True
        tc.variables["ZLINK_FRAMEWORK_CPP_WITH_ETCD"] = bool(self.options.with_etcd)
        tc.variables["ZLINK_FRAMEWORK_CPP_STAGE_STANDALONE_DEPENDENCIES"] = False
        for suffix in ("TESTS", "FOUNDATION_TESTS", "SAMPLES", "E2E", "CROSS_LANGUAGE"):
            tc.variables["ZLINK_FRAMEWORK_CPP_BUILD_" + suffix] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT"] = True
        for engine in ("UNREAL", "GODOT", "AXMOL"):
            tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_" + engine] = False
        # Conan supplies Core and the binding as dependencies. Their package
        # folders still identify the exact inputs used by the Framework build.
        zlink_cpp_dep = self.dependencies["zlink-cpp"]
        tc.variables["ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX"] = \
            zlink_cpp_dep.package_folder.replace("\\", "/")
        zlink_core_dep = self.dependencies["zlink"]
        tc.variables["ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX"] = \
            zlink_core_dep.package_folder.replace("\\", "/")
        generate_framework_cmake(self, tc)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        CMake(self).install()
        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        # Installed CMake configs own all component targets and dependency edges.
        # CMakeDeps must not synthesize a second framework component graph.
        self.cpp_info.set_property("cmake_find_mode", "none")
        self.cpp_info.builddirs = [
            "lib/cmake/zlink_framework",
            "lib/cmake/zlink_framework_cpp",
            "lib/cmake/zlink_http_client_cpp",
            "lib/cmake/zlink_stream_connector_cpp",
        ]
