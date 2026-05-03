#!/usr/bin/env python3

if __name__ != "__main__":
    raise SystemExit(f'Utility script "{__file__}" should not be used as a module!')

import argparse
import os
import shutil
import sys
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../"))

from misc.utility.color import Ansi, color_print

parser = argparse.ArgumentParser(description="Install NVIDIA Streamline SDK into Godot build_deps (Windows).")
parser.add_argument(
    "--extract_to",
    default="",
    help="Directory to extract the SDK into (must match SCons `streamline_sdk_path`). "
    "Default: <build_deps>/streamline_sdk. The official zip has `bin/x64` at its root.",
)
args = parser.parse_args()

if sys.platform != "win32":
    color_print(f"{Ansi.YELLOW}Streamline is Windows-only; nothing to install on this platform.")
    raise SystemExit(0)

# Base Godot dependencies path — same logic as `install_d3d12_sdk_windows.py`.
deps_folder = os.getenv("LOCALAPPDATA")
if deps_folder:
    deps_folder = os.path.join(deps_folder, "Godot", "build_deps")
else:
    deps_folder = os.path.join("bin", "build_deps")

# Default extract location — keep in sync with `streamline_sdk_path` in `platform/windows/detect.py`.
# Releases: https://github.com/NVIDIA-RTX/Streamline/releases
streamline_version = "2.10.3"
streamline_tag = f"v{streamline_version}"
streamline_zip_name = f"streamline-sdk-v{streamline_version}.zip"
streamline_url = (
    f"https://github.com/NVIDIA-RTX/Streamline/releases/download/{streamline_tag}/{streamline_zip_name}"
)
streamline_archive = os.path.join(deps_folder, streamline_zip_name)
streamline_extract = (
    os.path.normpath(os.path.expanduser(args.extract_to))
    if args.extract_to
    else os.path.join(deps_folder, "streamline_sdk")
)

if not os.path.exists(deps_folder):
    os.makedirs(deps_folder)
extract_parent = os.path.dirname(streamline_extract)
if extract_parent and not os.path.isdir(extract_parent):
    os.makedirs(extract_parent, exist_ok=True)


def _has_bin_directory(root: str) -> bool:
    for dirpath, _dirnames, _filenames in os.walk(root):
        if os.path.basename(dirpath).casefold() == "bin":
            return True
    return False


color_print(f"{Ansi.BOLD}Streamline SDK ({streamline_tag})")

if os.path.isfile(streamline_archive):
    os.remove(streamline_archive)
print(f"Downloading {streamline_zip_name} ...")
urllib.request.urlretrieve(streamline_url, streamline_archive)

if os.path.exists(streamline_extract):
    print(f"Removing existing Streamline SDK at {streamline_extract} ...")
    shutil.rmtree(streamline_extract)
os.makedirs(streamline_extract, exist_ok=True)
print(f"Extracting to {streamline_extract} ...")
shutil.unpack_archive(streamline_archive, streamline_extract, "zip")
os.remove(streamline_archive)

if not _has_bin_directory(streamline_extract):
    color_print(
        f"{Ansi.RED}Archive unpacked but no `bin` folder was found — the zip layout may have changed or the download is corrupt."
    )
    raise SystemExit(1)

color_print(f'{Ansi.GREEN}Streamline SDK {streamline_tag} extracted to "{streamline_extract}".')
color_print(
    f"{Ansi.GREEN}Rebuild with install_streamline_sdk=yes (default) so DLLs from bin/x64 are copied next to godot.exe."
)
