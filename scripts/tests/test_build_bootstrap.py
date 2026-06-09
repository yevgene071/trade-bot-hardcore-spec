#!/usr/bin/env python3
"""
Unit tests for scripts/bootstrap-build-env.sh and scripts/build.sh.

Tests use temporary directories, isolated CONAN_HOME, and fake tool shims
to validate:
  - Missing-tool diagnostics (exit 1 with actionable guidance)
  - Conan major-version validation (reject Conan 1.x)
  - Executable-bit expectations on scripts
  - Idempotent remote/profile command selection
  - build.sh --no-conan / --quick guard behavior
"""

import os
import shutil
import stat
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

# Resolve paths relative to this test file
REPO_ROOT = Path(__file__).resolve().parents[2]
BOOTSTRAP = REPO_ROOT / "scripts" / "bootstrap-build-env.sh"
BUILD_SH = REPO_ROOT / "scripts" / "build.sh"
TEST_SH = REPO_ROOT / "scripts" / "test.sh"

# Always include the directory containing bash so subprocess can find it
BASH_DIR = os.path.dirname(shutil.which("bash") or "/usr/bin/bash")


def run_script(script: str, env: dict | None = None, timeout: int = 30) -> subprocess.CompletedProcess:
    """Run a bash script with optional env overrides and return the result."""
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(
        ["bash", script],
        capture_output=True,
        text=True,
        env=merged_env,
        timeout=timeout,
    )


def make_path(*dirs: str) -> str:
    """Build a PATH that always includes bash's directory."""
    parts = list(dirs)
    if BASH_DIR not in parts:
        parts.append(BASH_DIR)
    return os.pathsep.join(parts)


class TestScriptExecutableBits(unittest.TestCase):
    """Ensure build scripts are executable."""

    def test_bootstrap_is_executable(self):
        st = BOOTSTRAP.stat()
        self.assertTrue(st.st_mode & stat.S_IXUSR, f"{BOOTSTRAP} is not executable")

    def test_build_sh_is_executable(self):
        st = BUILD_SH.stat()
        self.assertTrue(st.st_mode & stat.S_IXUSR, f"{BUILD_SH} is not executable")

    def test_test_sh_is_executable(self):
        st = TEST_SH.stat()
        self.assertTrue(st.st_mode & stat.S_IXUSR, f"{TEST_SH} is not executable")


class TestBootstrapMissingTools(unittest.TestCase):
    """Verify bootstrap fails fast with actionable messages when tools are missing."""

    def test_missing_cmake_fails(self):
        """Bootstrap should fail when cmake is not on PATH."""
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_bin = Path(tmpdir) / "bin"
            fake_bin.mkdir()
            env = {
                "PATH": make_path(str(fake_bin)),
                "HOME": tmpdir,
                "CONAN_HOME": str(Path(tmpdir) / ".conan2"),
            }
            result = run_script(str(BOOTSTRAP), env=env)
            self.assertNotEqual(result.returncode, 0, "Expected non-zero exit for missing tools")
            combined = result.stdout + result.stderr
            self.assertIn("cmake", combined.lower(), "Should mention cmake in error output")

    def test_missing_ninja_fails(self):
        """Bootstrap should fail when ninja is not on PATH."""
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_bin = Path(tmpdir) / "bin"
            fake_bin.mkdir()
            # Provide a fake cmake but not ninja
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/bin/bash\necho 'cmake version 3.28.0'\n")
            fake_cmake.chmod(0o755)
            env = {
                "PATH": make_path(str(fake_bin)),
                "HOME": tmpdir,
                "CONAN_HOME": str(Path(tmpdir) / ".conan2"),
            }
            result = run_script(str(BOOTSTRAP), env=env)
            self.assertNotEqual(result.returncode, 0)
            combined = result.stdout + result.stderr
            self.assertIn("ninja", combined.lower())

    def test_missing_compiler_fails(self):
        """Bootstrap should fail when no C++ compiler is on PATH."""
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_bin = Path(tmpdir) / "bin"
            fake_bin.mkdir()
            # Provide cmake, ninja, conan but no compiler
            for tool in ["cmake", "ninja"]:
                f = fake_bin / tool
                f.write_text(f"#!/bin/bash\necho '{tool} version 1.0'\n")
                f.chmod(0o755)
            fake_conan = fake_bin / "conan"
            fake_conan.write_text("#!/bin/bash\necho 'Conan version 2.29.0'\n")
            fake_conan.chmod(0o755)
            env = {
                "PATH": make_path(str(fake_bin)),
                "HOME": tmpdir,
                "CONAN_HOME": str(Path(tmpdir) / ".conan2"),
            }
            result = run_script(str(BOOTSTRAP), env=env)
            self.assertNotEqual(result.returncode, 0)
            combined = result.stdout + result.stderr
            self.assertTrue(
                any(word in combined.lower() for word in ["compiler", "g++", "clang++"]),
                f"Expected compiler guidance in output: {combined}",
            )


class TestConanVersionValidation(unittest.TestCase):
    """Verify bootstrap rejects Conan 1.x."""

    def test_conan_major_version_1_rejected(self):
        """Bootstrap should fail with a clear message when Conan 1.x is detected."""
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_bin = Path(tmpdir) / "bin"
            fake_bin.mkdir()

            for tool, version_output in [
                ("cmake", "cmake version 3.28.0"),
                ("ninja", "1.13.2"),
                ("g++", "g++ (GCC) 14.2.0"),
            ]:
                f = fake_bin / tool
                if tool == "g++":
                    f.write_text("#!/bin/bash\nif [[ \"$1\" == \"-dumpversion\" ]]; then echo '14'; else echo 'g++ (GCC) 14.2.0'; fi\n")
                else:
                    f.write_text(f"#!/bin/bash\necho '{version_output}'\n")
                f.chmod(0o755)

            fake_conan = fake_bin / "conan"
            fake_conan.write_text("#!/bin/bash\necho 'Conan version 1.64.1'\n")
            fake_conan.chmod(0o755)

            env = {
                "PATH": make_path(str(fake_bin)),
                "HOME": tmpdir,
                "CONAN_HOME": str(Path(tmpdir) / ".conan2"),
            }
            result = run_script(str(BOOTSTRAP), env=env)
            self.assertNotEqual(result.returncode, 0)
            combined = result.stdout + result.stderr
            self.assertIn("2", combined, "Should mention Conan 2.x requirement")

    def test_conan_major_version_2_accepted(self):
        """Bootstrap should accept Conan 2.x (version check only)."""
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_bin = Path(tmpdir) / "bin"
            fake_bin.mkdir()

            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text(textwrap.dedent("""\
                #!/bin/bash
                if [[ "$1" == "--help" ]]; then
                    echo "Ninja"
                    echo "Unix Makefiles"
                else
                    echo "cmake version 3.28.0"
                fi
            """))
            fake_cmake.chmod(0o755)

            fake_ninja = fake_bin / "ninja"
            fake_ninja.write_text("#!/bin/bash\necho '1.13.2'\n")
            fake_ninja.chmod(0o755)

            fake_conan = fake_bin / "conan"
            fake_conan.write_text(textwrap.dedent("""\
                #!/bin/bash
                if [[ "$1" == "--version" ]]; then
                    echo "Conan version 2.29.0"
                elif [[ "$1" == "profile" ]]; then
                    echo "[settings]
arch=x86_64
build_type=Release
compiler=gcc
compiler.cppstd=gnu23
compiler.libcxx=libstdc++11
compiler.version=14
os=Linux"
                elif [[ "$1" == "remote" ]]; then
                    echo "conancenter: https://center2.conan.io [Verify SSL: True, Enabled: True]"
                fi
            """))
            fake_conan.chmod(0o755)

            fake_gpp = fake_bin / "g++"
            fake_gpp.write_text(textwrap.dedent("""\
                #!/bin/bash
                if [[ "$1" == "-dumpversion" ]]; then
                    echo "14"
                else
                    echo "g++ (GCC) 14.2.0"
                fi
            """))
            fake_gpp.chmod(0o755)

            env = {
                "PATH": make_path(str(fake_bin)),
                "HOME": tmpdir,
                "CONAN_HOME": str(Path(tmpdir) / ".conan2"),
            }
            result = run_script(str(BOOTSTRAP), env=env)
            combined = result.stdout + result.stderr
            self.assertNotIn("incompatible", combined.lower(),
                             "Should not reject Conan 2.x")
            self.assertNotIn("Conan 2.x is required but found", combined,
                             "Should not show Conan version error for 2.x")


class TestConanRemoteConfiguration(unittest.TestCase):
    """Verify idempotent Conan remote configuration logic."""

    def test_remote_update_command_structure(self):
        """The bootstrap script should contain logic to update or add the conancenter remote."""
        script_content = BOOTSTRAP.read_text()
        self.assertIn("center2.conan.io", script_content,
                      "Script should reference the correct Conan 2 remote URL")
        self.assertIn("conan remote update", script_content,
                      "Script should contain 'conan remote update' for existing remotes")
        self.assertIn("conan remote add", script_content,
                      "Script should contain 'conan remote add' for missing remotes")

    def test_profile_detect_guard(self):
        """Script should only run 'conan profile detect --force' when profile is missing."""
        script_content = BOOTSTRAP.read_text()
        # Conan 2.x uses 'conan profile show' (without 'default') to check default profile
        self.assertIn("conan profile show", script_content,
                      "Script should check for existing profile first")
        self.assertIn("conan profile detect --force", script_content,
                      "Script should use --force to avoid interactive prompts")


class TestBuildShFlags(unittest.TestCase):
    """Verify build.sh flag parsing and behavior."""

    def test_build_sh_has_no_conan_flag(self):
        """build.sh should support --no-conan flag."""
        content = BUILD_SH.read_text()
        self.assertIn("--no-conan", content)

    def test_build_sh_has_quick_flag(self):
        """build.sh should support --quick flag."""
        content = BUILD_SH.read_text()
        self.assertIn("--quick", content)

    def test_build_sh_has_tests_flag(self):
        """build.sh should support --tests flag."""
        content = BUILD_SH.read_text()
        self.assertIn("--tests", content)

    def test_build_sh_has_clean_mode(self):
        """build.sh should support 'clean' mode."""
        content = BUILD_SH.read_text()
        self.assertIn('"clean"', content)

    def test_build_sh_calls_bootstrap(self):
        """build.sh should invoke bootstrap-build-env.sh before conan install."""
        content = BUILD_SH.read_text()
        self.assertIn("bootstrap-build-env.sh", content,
                      "build.sh should call bootstrap-build-env.sh")

    def test_build_sh_conan_failure_not_masked(self):
        """build.sh should not mask conan install failures with || true on the entire command."""
        content = BUILD_SH.read_text()
        self.assertIn("conan install", content,
                      "build.sh should run conan install")
        # Ensure the conan install command does NOT end with '|| true'
        # which would mask failures
        lines = content.split('\n')
        in_conan_block = False
        for line in lines:
            stripped = line.strip()
            if 'conan install core' in stripped:
                in_conan_block = True
            if in_conan_block and stripped.endswith('|| true'):
                self.fail("conan install should not end with '|| true' — failures must propagate")
            if in_conan_block and (stripped.startswith('|| die') or 'Conan install failed' in stripped):
                break  # Found proper error handling

    def test_build_sh_verifies_toolchain_file(self):
        """build.sh should verify conan_toolchain.cmake was generated after conan install."""
        content = BUILD_SH.read_text()
        self.assertIn("conan_toolchain.cmake", content,
                      "build.sh should check for conan_toolchain.cmake after conan install")

    def test_build_sh_quick_guard_checks_toolchain(self):
        """build.sh --quick should fail if conan_toolchain.cmake is missing."""
        content = BUILD_SH.read_text()
        # The quick guard should check for conan_toolchain.cmake, not just CMakeCache
        quick_section = content[content.find('--quick requested'):] if '--quick requested' in content else ''
        self.assertIn("conan_toolchain.cmake", quick_section,
                      "--quick guard should check for conan_toolchain.cmake")

    def test_build_sh_proxy_support(self):
        """build.sh should support CONAN_PROXY_URL environment variable."""
        content = BUILD_SH.read_text()
        self.assertIn("CONAN_PROXY_URL", content,
                      "build.sh should support CONAN_PROXY_URL env var")

    def test_build_sh_exits_on_unknown_option(self):
        """build.sh should handle unknown options."""
        result = subprocess.run(
            ["bash", str(BUILD_SH), "--bogus-flag"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertNotEqual(result.returncode, 0, "Unknown flag should cause non-zero exit")


class TestBootstrapIdempotency(unittest.TestCase):
    """Verify that the bootstrap script structure supports idempotent execution."""

    def test_script_has_set_eu(self):
        """Script should use strict bash mode."""
        content = BOOTSTRAP.read_text()
        self.assertIn("set -euo pipefail", content,
                      "Script must use strict bash mode")

    def test_script_checks_before_mutation(self):
        """Script should check current state before making changes."""
        content = BOOTSTRAP.read_text()
        # Profile check - Conan 2.x uses 'conan profile show' (without 'default')
        self.assertIn("conan profile show", content,
                      "Should check existing profile before detect")
        self.assertNotIn("conan profile show default", content,
                         "Should use Conan 2.x syntax (no 'default' arg)")
        # Remote check
        self.assertIn("conan remote list", content,
                      "Should check existing remotes before updating")


class TestBootstrapRunsCleanly(unittest.TestCase):
    """Test bootstrap against the real environment (if available)."""

    def test_bootstrap_succeeds_in_real_env(self):
        """Run bootstrap in the real environment — should succeed if tools are installed."""
        result = run_script(str(BOOTSTRAP))
        if result.returncode != 0:
            self.fail(
                f"bootstrap-build-env.sh failed in real environment.\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )


class TestBootstrapProxySupport(unittest.TestCase):
    """Verify proxy configuration support in bootstrap script."""

    def test_bootstrap_has_proxy_config(self):
        """Bootstrap should support CONAN_PROXY_URL environment variable."""
        content = BOOTSTRAP.read_text()
        self.assertIn("CONAN_PROXY_URL", content,
                      "Bootstrap should support CONAN_PROXY_URL env var")

    def test_bootstrap_exports_http_proxy(self):
        """Bootstrap should export HTTP_PROXY when CONAN_PROXY_URL is set."""
        content = BOOTSTRAP.read_text()
        self.assertIn("HTTP_PROXY", content,
                      "Bootstrap should set HTTP_PROXY from CONAN_PROXY_URL")
        self.assertIn("HTTPS_PROXY", content,
                      "Bootstrap should set HTTPS_PROXY from CONAN_PROXY_URL")

    def test_bootstrap_respects_existing_proxy(self):
        """Bootstrap should use existing HTTP_PROXY if set and CONAN_PROXY_URL is not."""
        content = BOOTSTRAP.read_text()
        self.assertIn("HTTP_PROXY", content,
                      "Bootstrap should detect existing HTTP_PROXY")


class TestCIWorkflowAlignment(unittest.TestCase):
    """Verify CI workflow uses the same bootstrap/build path as local worktrees."""

    CI_PATH = REPO_ROOT / ".github" / "workflows" / "ci.yml"

    def _read_ci(self) -> str:
        if not self.CI_PATH.exists():
            self.skipTest(f"CI workflow not found at {self.CI_PATH}")
        return self.CI_PATH.read_text()

    def test_ci_uses_checkout_v4(self):
        """CI should use actions/checkout@v4 or higher."""
        content = self._read_ci()
        self.assertRegex(
            content, r'actions/checkout@v[4-9]',
            "CI should use actions/checkout@v4 or later")

    def test_ci_installs_ninja(self):
        """CI should install Ninja via apt."""
        content = self._read_ci()
        self.assertIn("ninja-build", content,
                      "CI should install ninja-build package")

    def test_ci_installs_gcc_14(self):
        """CI should explicitly install GCC 14+ (not rely on build-essential)."""
        content = self._read_ci()
        self.assertRegex(
            content, r'g\+\+-14|gcc-14|g\+\+-15|clang\+\+-17|clang\+\+-18',
            "CI should explicitly install GCC 14+ or Clang 17+")
        self.assertIn("update-alternatives", content,
                      "CI should use update-alternatives to select the compiler")

    def test_ci_installs_conan_via_pip_or_pipx(self):
        """CI should install Conan 2 via pipx or pip (not a distro package)."""
        content = self._read_ci()
        self.assertRegex(
            content, r'pip[x]?\s+install.*conan',
            "CI should install Conan via pipx or pip")

    def test_ci_runs_bootstrap(self):
        """CI should invoke bootstrap-build-env.sh."""
        content = self._read_ci()
        self.assertIn("bootstrap-build-env.sh", content,
                      "CI should run the bootstrap script")

    def test_ci_runs_build_sh(self):
        """CI should use scripts/build.sh for the build step."""
        content = self._read_ci()
        self.assertIn("build.sh", content,
                      "CI should use the canonical build.sh")

    def test_ci_runs_test_sh(self):
        """CI should use scripts/test.sh for the test step."""
        content = self._read_ci()
        self.assertIn("test.sh", content,
                      "CI should use the canonical test.sh")

    def test_ci_has_conan_cache(self):
        """CI should cache Conan packages."""
        content = self._read_ci()
        self.assertIn("actions/cache", content,
                      "CI should use actions/cache for Conan packages")
        self.assertIn(".conan2", content,
                      "CI should cache the .conan2 directory")

    def test_ci_has_conan_proxy_support(self):
        """CI should set CONAN_PROXY_URL from secrets for dependency resolution."""
        content = self._read_ci()
        self.assertIn("CONAN_PROXY_URL", content,
                      "CI should reference CONAN_PROXY_URL for proxy")
        self.assertIn("secrets.CONAN_PROXY_URL", content,
                      "CI should source proxy from GitHub secrets")

    def test_ci_cache_key_includes_conanfile(self):
        """Cache key should include conanfile.txt hash for proper invalidation."""
        content = self._read_ci()
        self.assertIn("conanfile.txt", content,
                      "Cache key should include conanfile.txt hash")

    def test_ci_cache_key_includes_compiler(self):
        """Cache key should include compiler version to avoid stale binary packages."""
        content = self._read_ci()
        # Look for gcc14, clang17, etc. in the cache key
        self.assertRegex(
            content, r'key:.*conan.*-gcc|key:.*conan.*-clang',
            "Cache key should include compiler identifier")


class TestBuildShProxyIntegration(unittest.TestCase):
    """Verify build.sh proxy support passes through to conan install."""

    def test_build_sh_proxy_env_passed_to_conan(self):
        """build.sh should pass CONAN_PROXY_URL as HTTP_PROXY to conan install."""
        content = BUILD_SH.read_text()
        self.assertIn("CONAN_PROXY_URL", content,
                      "build.sh should support CONAN_PROXY_URL")
        self.assertIn("HTTP_PROXY", content,
                      "build.sh should set HTTP_PROXY from CONAN_PROXY_URL")
        self.assertIn("HTTPS_PROXY", content,
                      "build.sh should set HTTPS_PROXY from CONAN_PROXY_URL")

    def test_build_sh_proxy_default_empty(self):
        """build.sh should default CONAN_PROXY_URL to empty string (not a hardcoded value)."""
        content = BUILD_SH.read_text()
        # Look for the default assignment (with or without quotes)
        self.assertRegex(
            content, r'CONAN_PROXY_URL=.*\$\{CONAN_PROXY_URL:-\}',
            "build.sh should default CONAN_PROXY_URL to empty using parameter expansion")
        # Ensure no hardcoded proxy URL
        self.assertNotRegex(
            content, r'CONAN_PROXY_URL=["\']https?://',
            "build.sh should not hardcode a proxy URL")


class TestCIConcurrency(unittest.TestCase):
    """Verify CI workflow has concurrency control."""

    CI_PATH = REPO_ROOT / ".github" / "workflows" / "ci.yml"

    def test_ci_has_concurrency_group(self):
        """CI should cancel in-flight runs for the same branch."""
        if not self.CI_PATH.exists():
            self.skipTest("CI workflow not found")
        content = self.CI_PATH.read_text()
        self.assertIn("concurrency", content,
                      "CI should have concurrency control")
        self.assertIn("cancel-in-progress", content,
                      "CI should cancel in-progress runs")


if __name__ == "__main__":
    unittest.main()
