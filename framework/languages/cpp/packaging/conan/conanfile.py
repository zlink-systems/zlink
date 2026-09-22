"""Conan recipe for the zlink C++ framework (HTTP client and stream connector)."""

import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get

required_conan_version = ">=2.1"

# This is the authoritative third-party name/version list for the framework.
# bootstrap.cmake reads it from the released archive, so its CMakeDeps graph is
# identical to the package graph without downloading zlink-cpp (the bootstrap
# builds that first-party archive itself). vcpkg.json remains the explicit
# vcpkg fallback manifest: its port feature names are vcpkg-specific.
ZLINK_FRAMEWORK_CPP_THIRD_PARTY_REQUIREMENTS = (
    # CMakeLists.txt:22 unconditionally needs the api component.
    "opentelemetry-cpp/1.26.0",
    "boost/1.85.0",
    "nlohmann_json/3.11.3",
    "openssl/[>=3.0 <4]",
    "lz4/1.9.4",
    "protobuf/5.27.0",
    # The async option below owns its compatible libuv transitively. 1.3.15 is the
    # first ConanCenter recipe whose async build finds libuv without a system
    # libuv-dev (1.3.13 fails with "uv.h: No such file" on a clean machine).
    "redis-plus-plus/1.3.15",
)


class ZlinkFrameworkConan(ConanFile):
    name = "zlink-framework"
    version = "0.22.0"
    package_type = "static-library"
    license = "FSL-1.1-ALv2"
    homepage = "https://github.com/zlink-systems/zlink"
    description = "C++ framework, HTTP client and stream connector for zlink"
    settings = "os", "arch", "compiler", "build_type"
    options = {"fPIC": [True, False]}
    default_options = {
        "fPIC": True,
        "boost/*:header_only": True,
        # Mirrors vcpkg.json's redis-plus-plus async-std feature.
        "redis-plus-plus/*:build_async": True,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        self.requires("zlink-cpp/1.4.0", transitive_headers=True,
                      transitive_libs=True)
        for dependency in ZLINK_FRAMEWORK_CPP_THIRD_PARTY_REQUIREMENTS:
            self.requires(dependency, transitive_headers=True, transitive_libs=True)

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
                    + ". Publish the output of cmake/prepare-source-archive.cmake "
                    "and update conandata.yml."
                )

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["CMAKE_INSTALL_LIBDIR"] = "lib"
        tc.variables["ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST"] = True
        tc.variables["ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK"] = True
        for suffix in ("TESTS", "FOUNDATION_TESTS", "SAMPLES", "E2E", "CROSS_LANGUAGE"):
            tc.variables["ZLINK_FRAMEWORK_CPP_BUILD_" + suffix] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT"] = True
        for engine in ("UNREAL", "GODOT", "AXMOL"):
            tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_" + engine] = False
        # CMakeLists.txt's stream-connector staging step does not rely on the
        # find_package(zlink_cpp CONFIG) result alone: it separately globs the
        # binding's and Core's link libraries out of
        # ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX / _CORE_PREFIX, two CACHE
        # PATH variables that otherwise default to a workspace-relative
        # ".artifacts/wsl/install/..." layout meant for the local-package
        # workflow. Under Conan there is no such workspace tree next to the
        # extracted source archive, so that glob finds nothing and CMake dies
        # with "has no link library" at configure time. Point both prefixes at
        # the actual Conan package folders so the existing override mechanism
        # picks them up.
        zlink_cpp_dep = self.dependencies["zlink-cpp"]
        tc.variables["ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX"] = \
            zlink_cpp_dep.package_folder.replace("\\", "/")
        zlink_core_dep = self.dependencies["zlink"]
        tc.variables["ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX"] = \
            zlink_core_dep.package_folder.replace("\\", "/")
        tc.generate()
        CMakeDeps(self).generate()

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
