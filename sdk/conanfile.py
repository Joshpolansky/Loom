from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout
from conan.tools.files import rmdir
import os


class LoomSdkConan(ConanFile):
    name = "loom"
    # version is set dynamically by set_version() below
    package_type = "header-library"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "include/*", "loom-config.cmake.in", "CMakeLists.txt"
    generators = "CMakeDeps", "CMakeToolchain"

    def set_version(self):
        # Read from the repo-root VERSION file when in the source tree.
        # When loaded from the Conan package cache, self.recipe_folder points to the cached
        # recipe dir and VERSION won't exist — in that case, the version is already known
        # from the package reference and set_version() can safely no-op.
        version_file = os.path.join(self.recipe_folder, "..", "VERSION")
        if os.path.isfile(version_file):
            with open(version_file) as f:
                self.version = f.read().strip()

    def requirements(self):
        self.requires("glaze/7.2.0")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        # Pass version as -D arg: toolchain vars are only available after project(), but
        # cmake command-line -D args land in the initial cache before CMakeLists.txt runs.
        cmake.configure(variables={"LOOM_SDK_VERSION": self.version})
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

        # Cleanup CMake transient files if present
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake", "CMakeFiles"))

    def package_id(self):
        # Header-only: package id should not depend on settings
        self.info.clear()

    def package_info(self):
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.set_property("cmake_file_name", "loom")
        self.cpp_info.set_property("cmake_target_name", "loom::sdk")
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
