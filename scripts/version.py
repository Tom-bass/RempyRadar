import subprocess
Import("env")

try:
    version = subprocess.check_output(
        ["git", "describe", "--tags", "--exact-match"],
        stderr=subprocess.DEVNULL,
        cwd=env.subst("$PROJECT_DIR")
    ).decode().strip()
except subprocess.CalledProcessError:
    version = "dev"

print(f"RempyRadar firmware version: {version}")
env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(version))])
