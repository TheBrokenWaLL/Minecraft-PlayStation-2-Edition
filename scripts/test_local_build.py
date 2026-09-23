"""Launcher smoke tests; never invoke a compiler or download dependencies."""
import os
import shutil
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LauncherTests(unittest.TestCase):
    def invoke(self, *args, env=None):
        return subprocess.run(['bash', str(ROOT / 'build_linux.sh'), *args],
                              cwd='/tmp', env=env, text=True, capture_output=True)

    def test_platform_commands(self):
        for target, marker in [('gcc', '-DCMAKE_CXX_COMPILER=g++'),
                               ('wii', '-DWII_BRINGUP=OFF'),
                               ('ps2', '-DPS2_ENABLE_NETWORK=ON')]:
            with self.subTest(target=target):
                result = self.invoke(target, '--dry-run', '--jobs', '2')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(marker, result.stdout)
                self.assertIn('--target OptiCraft', result.stdout)
                self.assertIn('--parallel 2', result.stdout)
                if target != 'gcc':
                    self.assertIn('--target ' + target + '-data', result.stdout)

    def test_configure_only_and_no_assets(self):
        result = self.invoke('wii', '--dry-run', '--configure-only')
        self.assertEqual(result.returncode, 0)
        self.assertNotIn('--build', result.stdout)
        result = self.invoke('ps2', '--dry-run', '--no-assets', '--debug')
        self.assertEqual(result.returncode, 0)
        self.assertIn('-DCMAKE_BUILD_TYPE=Debug', result.stdout)
        self.assertNotIn('--target ps2-data', result.stdout)

    def test_short_configuration_arguments(self):
        for target in ('gcc', 'wii', 'ps2'):
            result = self.invoke(target, '-debug', '--dry-run')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('-DCMAKE_BUILD_TYPE=Debug', result.stdout)
        self.assertNotEqual(self.invoke('gcc', '-debug', '-release').returncode, 0)

    @unittest.skipUnless(shutil.which('pwsh'), 'PowerShell 7 is not installed')
    def test_windows_command_generation(self):
        for target in ('gcc', 'wii', 'ps2'):
            result = subprocess.run(['pwsh', '-NoProfile', '-File',
                str(ROOT / 'build_windows.ps1'), target, '-Debug', '-DryRun'],
                text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('-DCMAKE_BUILD_TYPE=Debug', result.stdout)
            self.assertIn('OptiCraft', result.stdout)

    def test_invalid_arguments(self):
        for args in [('gcc', '--jobs', '0'), ('wii', '--jobs'),
                     ('gcc', 'ps2'), ('gcc', '--unknown')]:
            self.assertNotEqual(self.invoke(*args).returncode, 0)

    def test_failure_through_log_pipeline(self):
        # Use the existing dependency files but never configure/build anything.
        # Stub cmake deliberately fails; no executable path should be reported.
        with tempfile.TemporaryDirectory(prefix='opticraft-launcher-test-') as directory:
            stub = Path(directory) / 'cmake'
            stub.write_text('#!/bin/sh\necho deliberate-configure-failure\nexit 23\n')
            stub.chmod(0o755)
            env = dict(os.environ, PATH=directory + os.pathsep + os.environ['PATH'])
            result = self.invoke('gcc', '--no-assets', env=env)
            self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
            self.assertIn('deliberate-configure-failure', result.stdout)
            self.assertNotIn('Executable:', result.stdout)


if __name__ == '__main__':
    unittest.main()
