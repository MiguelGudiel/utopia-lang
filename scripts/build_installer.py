import os
import sys
import subprocess
import argparse
from pathlib import Path

def load_env(env_path: Path):
    env_vars = {}
    if not env_path.exists():
        print(f"Warning: The file {env_path.name} was not found. Proceeding without it.")
        return env_vars
        
    with open(env_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            if '=' in line:
                key, value = line.split('=', 1)
                key = key.strip()
                value = value.strip().strip('"').strip("'")
                env_vars[key] = value
                
    return env_vars

def run_command(cmd: list, cwd: Path):
    cmd_str = ' '.join(cmd)
    print(f"\n>> Executing: {cmd_str}")
    
    result = subprocess.run(cmd, cwd=cwd)
    if result.returncode != 0:
        print(f"\n[ERROR] The command failed with code {result.returncode}")
        sys.exit(result.returncode)

def main():
    parser = argparse.ArgumentParser(description="Utopia Installer Builder")
    parser.add_argument('--arch', type=str, default="", help="Target architecture (x64, arm64, etc.)")
    args = parser.parse_args()

    workspace_dir = Path(__file__).resolve().parent.parent
    env_file = workspace_dir / "release.env"
    
    env_vars = load_env(env_file)

    cmake_config_cmd = [
      "cmake",
      "-S", ".",
      "-B", "build_release",
      "-DCMAKE_BUILD_TYPE=Release",
      "-DENABLE_SANITIZERS=OFF"
    ]

    for key, value in env_vars.items():
        cmake_config_cmd.append(f"-D{key}={value}")

    platform = sys.platform
    cpack_generator = ""

    if platform == "win32":
        arch = args.arch if args.arch else "x64"
        cmake_config_cmd.extend(["-A", arch])
        cpack_generator = "NSIS"
        
    elif platform == "darwin":
        arch = args.arch if args.arch else "arm64"
        cmake_config_cmd.append(f"-DCMAKE_OSX_ARCHITECTURES={arch}")
        cpack_generator = "DragNDrop"
        
    elif platform.startswith("linux"):
        cpack_generator = "RPM"
        
    else:
        print(f"[ERROR] Unsupported platform: {platform}")
        sys.exit(1)

    run_command(cmake_config_cmd, cwd=workspace_dir)

    run_command(["cmake", "--build", "build_release", "--config", "Release"], cwd=workspace_dir)

    run_command([
        "cpack", 
        "--config", "build_release/CPackConfig.cmake", 
        "-C", "Release", 
        "-G", cpack_generator
    ], cwd=workspace_dir)
    
    print("\n[SUCCESS] Installer successfully generated in the build_release folder./.")

if __name__ == "__main__":
    main()