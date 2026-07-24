"""
thirdparty_fetch.py - Generic helper for fetching SDK/thirdparty zip packages.

Usage in SConstruct / SCsub:
    from misc.utility.thirdparty_fetch import fetch_zip

    fetch_zip(
        name       = "NVIDIA Nsight Aftermath SDK",
        url        = "https://...",
        dest_dir   = "thirdparty/aftermath/include",
        zip_subdir = "include",   # only extract this subtree from the zip
    )

All parameters except name/url/dest_dir are optional.
Returns True if headers are available (already present or freshly downloaded),
False if the download failed.

Re-extraction is triggered automatically when the URL changes.
"""

import io
import os
import shutil
import urllib.request
import zipfile

# Name of the file written inside dest_dir to record which URL was fetched.
_URL_STAMP = ".fetch_url"


def fetch_zip(
    name,
    url,
    dest_dir,
    zip_subdir=None,
):
    """
    Ensure that `dest_dir` contains the contents of the zip at `url`.

    name       - Human-readable SDK name used in log messages.
    url        - Direct download URL for the zip file.
    dest_dir   - Local directory to extract into.
    zip_subdir - If set, only entries whose zip path starts with this prefix
                 are extracted, and that prefix is stripped from dest paths.
                 E.g. zip_subdir="include" extracts include/foo.h -> dest_dir/foo.h.
    """
    prefix = (zip_subdir.rstrip("/") + "/") if zip_subdir else ""
    stamp = os.path.join(dest_dir, _URL_STAMP)

    os.makedirs(dest_dir, exist_ok=True)

    # Check whether the previously fetched URL matches the requested one.
    cached_url = None
    if os.path.isfile(stamp):
        with open(stamp, "r") as f:
            cached_url = f.read().strip()

    if cached_url == url:
        return True

    if cached_url is not None:
        print(f"{name}: URL changed - clearing '{dest_dir}' and re-fetching.")
        shutil.rmtree(dest_dir)
        os.makedirs(dest_dir, exist_ok=True)
    else:
        print(f"{name}: not found. Downloading from:\n  {url}")

    try:
        with urllib.request.urlopen(url) as resp:
            data = resp.read()

        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            extracted = 0
            for member in zf.namelist():
                if member.endswith("/"):
                    continue
                if prefix and not member.startswith(prefix):
                    continue
                rel = member[len(prefix) :]
                dest = os.path.join(dest_dir, rel)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with zf.open(member) as src, open(dest, "wb") as dst:
                    dst.write(src.read())
                extracted += 1

        # Write the stamp so future runs can detect URL changes.
        with open(stamp, "w") as f:
            f.write(url)

        print(f"{name}: extracted {extracted} file(s) to '{dest_dir}'.")
        return True

    except Exception as e:
        print(f"ERROR: Failed to fetch {name}: {e}")
        return False
