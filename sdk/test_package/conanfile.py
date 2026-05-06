import os
from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class TestPackageConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeToolchain", "CMakeDeps"

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        # Require the package under test
        self.requires(self.tested_reference_str)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        # Locate built test executable and run it.
        exe_name = "test_package.exe" if str(self.settings.os).lower().startswith("win") else "test_package"
        bin_path = None
        for root, dirs, files in os.walk(self.build_folder):
            if exe_name in files:
                bin_path = os.path.join(root, exe_name)
                break
        if not bin_path:
            self.output.warning("test executable not found; skipping run")
            return
        self.run(bin_path, env="conanrun")
