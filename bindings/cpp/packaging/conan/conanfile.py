"""Draft: the public source asset must be completed before conan create works."""

import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get

required_conan_version = ">=2.1"


class ZlinkCppConan(ConanFile):
    name = "zlink-cpp"
    version = "0.17.7"
    package_type = "static-library"
    license = "MPL-2.0"
    homepage = "https://github.com/zlink-systems/zlink"
    description = "C++20 binding for the zlink messaging library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        self.requires("zlink/0.17.5", transitive_headers=True, transitive_libs=True)

    def validate(self):
        if self.settings.compiler.get_safe("cppstd"):
            check_min_cppstd(self, "20")

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)
        if not os.path.isfile(os.path.join(self.source_folder, "CMakeLists.txt")):
            raise ConanInvalidConfiguration(
                "The published zlink-cpp archive is header-only; publish the complete "
                "bindings/cpp source unit plus LICENSE and update conandata.yml."
            )

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["ZLINK_CPP_BUILD_TESTS"] = False
        tc.variables["ZLINK_CPP_BUILD_SAMPLES"] = False
        tc.variables["ZLINK_CPP_BUILD_BENCHMARKS"] = False
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
        self.cpp_info.set_property("cmake_file_name", "zlink_cpp")
        self.cpp_info.set_property("cmake_target_name", "zlink::cpp")
        self.cpp_info.libs = ["zlink_cpp"]
