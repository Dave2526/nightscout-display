# PlatformIO-Prescript: Commit-Kürzel als GIT_COMMIT in die Firmware (+ "*" bei lokalen Änderungen)
import subprocess

Import("env")

def git(*args):
    return subprocess.check_output(["git", *args], cwd=env["PROJECT_DIR"], stderr=subprocess.DEVNULL).decode().strip()

try:
    commit = git("rev-parse", "--short", "HEAD")
    if git("status", "--porcelain", "--untracked-files=no"):
        commit += "*"
except Exception:
    commit = "ohne-git"  # z.B. aus dem ZIP gebaut

env.Append(CPPDEFINES=[("GIT_COMMIT", env.StringifyMacro(commit))])
