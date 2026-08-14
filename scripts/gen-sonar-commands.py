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
        "-Iprojects/evoplayer",
        "-Iprojects/evoplayer/pp/include",
        "-Iprojects/evoplayer/ui/include",
        "-Iprojects/evoplayer/media/include",
        "-Iprojects/evoplayer/addons/include",
        "-Iprojects/common/include",
        "-Iprojects/evoplayer/metadata",
        "-Iprojects/evoplayer/playback",
        "-Iprojects/evoplayer/storage",
    ]
    
    entries = []
    for file_path in c_files:
        rel_path = os.path.relpath(file_path, repo_root).replace("\\", "/")
        args = [
            "clang",
            "-std=c11",
            "-DNO_OPENSSL=1",
            "-DEVO_PLAYER_VERSION=\"0.8.0-dev\""
        ] + includes + ["-c", rel_path]
        
        entries.append({
            "directory": ".",
            "arguments": args,
            "file": rel_path
        })
        
    out_file = os.path.join(repo_root, "compile_commands.json")
    with open(out_file, "w") as f:
        json.dump(entries, f, indent=2)
        
    print(f"==> Generated compile_commands.json with {len(entries)} files for SonarCloud analysis")

if __name__ == "__main__":
    main()
