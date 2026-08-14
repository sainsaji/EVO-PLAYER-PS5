#!/usr/bin/env python3
"""
Generate compile_commands.json containing all EVO Player source files
for SonarCloud / SonarQube static analysis.
"""
import glob
import json
import os

def main():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    
    # Collect all C files in projects/evoplayer and tests
    c_files = sorted(
        glob.glob(os.path.join(repo_root, "projects/evoplayer/**/*.c"), recursive=True) +
        glob.glob(os.path.join(repo_root, "projects/evoplayer/*.c"), recursive=True) +
        glob.glob(os.path.join(repo_root, "tests/*.c"), recursive=True)
    )
    
    includes = [
        "-I" + os.path.join(repo_root, "projects/evoplayer"),
        "-I" + os.path.join(repo_root, "projects/evoplayer/pp/include"),
        "-I" + os.path.join(repo_root, "projects/evoplayer/ui/include"),
        "-I" + os.path.join(repo_root, "projects/evoplayer/media/include"),
        "-I" + os.path.join(repo_root, "projects/evoplayer/addons/include"),
        "-I" + os.path.join(repo_root, "projects/common/include"),
        "-I" + os.path.join(repo_root, "projects/evoplayer/metadata"),
        "-I" + os.path.join(repo_root, "projects/evoplayer/playback"),
        "-I" + os.path.join(repo_root, "projects/evoplayer/storage"),
    ]
    
    entries = []
    for file_path in c_files:
        obj_path = file_path + ".o"
        cmd = ["gcc", "-O0", "-g", "-Wall", "-std=gnu11", "-DNO_OPENSSL=1", "-DEVO_PLAYER_VERSION=\"0.8.0-dev\""] + includes + ["-c", file_path, "-o", obj_path]
        entries.append({
            "directory": repo_root,
            "command": " ".join(cmd),
            "file": file_path,
            "output": obj_path
        })
        
    out_file = os.path.join(repo_root, "compile_commands.json")
    with open(out_file, "w") as f:
        json.dump(entries, f, indent=2)
        
    print(f"==> Generated compile_commands.json with {len(entries)} files for SonarCloud analysis")

if __name__ == "__main__":
    main()
