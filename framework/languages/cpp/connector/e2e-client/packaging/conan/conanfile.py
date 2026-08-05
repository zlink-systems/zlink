from conan import ConanFile
from conan.tools.files import copy
import os


class ZlinkStreamE2EClientConan(ConanFile):
    name = "zlink-stream-e2e-client"
    version = "0.9.0"
    license = "MPL-2.0"
    url = "https://github.com/zlink-systems/zlink"
    description = "ZLink C++ Stream Connector scenario client for server e2e, smoke, and perf tests"
    package_type = "header-library"
    no_copy_source = True
    requires = "zlink-stream-connector/0.9.0"

    def export_sources(self):
        repo_root = os.path.abspath(
            os.path.join(self.recipe_folder, "../../../../../../../")
        )
        copy(
            self,
            "*.hpp",
            src=os.path.join(
                repo_root, "framework/languages/cpp/connector/e2e-client/include"
            ),
            dst=os.path.join(self.export_sources_folder, "include"),
        )
        copy(self, "LICENSE", src=repo_root, dst=self.export_sources_folder)

    def package_id(self):
        self.info.clear()

    def package(self):
        copy(
            self,
            "LICENSE",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "licenses"),
        )
        copy(
            self,
            "*.hpp",
            src=os.path.join(self.source_folder, "include"),
            dst=os.path.join(self.package_folder, "include"),
        )

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "zlink_stream_e2e_client")
        self.cpp_info.set_property("cmake_target_name", "zlink::stream_e2e_client")
        self.cpp_info.requires = ["zlink-stream-connector::zlink-stream-connector"]
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
