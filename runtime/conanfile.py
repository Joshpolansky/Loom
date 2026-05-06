from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy
import os


class LoomRuntimeConan(ConanFile):
    name = "loom-runtime"
    # version is set dynamically by set_version() below
    package_type = "static-library"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "CMakeLists.txt", "include/**", "src/**"
    generators = "CMakeDeps", "CMakeToolchain"

    def set_version(self):
        version_file = os.path.join(self.recipe_folder, "..", "VERSION")
        if os.path.isfile(version_file):
            with open(version_file) as f:
                self.version = f.read().strip()

    def requirements(self):
        self.requires(f"loom/{self.version}@local/stable", transitive_headers=True)
        self.requires("spdlog/1.17.0", transitive_headers=True)
        self.requires("crowcpp-crow/1.3.0", transitive_headers=True)

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure(variables={"LOOM_RUNTIME_VERSION": self.version})
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "loom-runtime")
        self.cpp_info.set_property("cmake_target_name", "loom::runtime")
        self.cpp_info.libs = ["loom_runtime"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.libdirs = ["lib"]

