CMAKE_KEYWORD = "project(Lbug VERSION "
CMAKE_SUFFIX = " LANGUAGES CXX C)\n"
EXTENSION_KEYWORD = 'add_definitions(-DLBUG_EXTENSION_VERSION="'
EXTENSION_SUFFIX = '")\n'
EXTENSION_DEV_VERSION = "dev"

import os
import re
import sys
from datetime import date


def read_base_version(cmake_lists):
    """Extract MAJOR.MINOR.PATCH from the project() line in CMakeLists.txt.

    Previous nightly runs may have written a four-component version such as
    0.15.3.20260409 (the date as the fourth component).  We always strip the
    fourth component so that successive nightly runs keep using the same
    MAJOR.MINOR.PATCH base, producing e.g. 0.15.3.dev20260410 today and
    0.15.3.dev20260411 tomorrow.
    """
    for line in cmake_lists:
        if CMAKE_KEYWORD in line:
            m = re.search(r"project\(Lbug VERSION ([\d.]+)", line)
            if m:
                parts = m.group(1).split(".")
                # Keep only the first three numeric components (MAJOR.MINOR.PATCH).
                return ".".join(parts[:3])
    return None


def main():
    cmake_lists_path = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "CMakeLists.txt")
    )

    with open(cmake_lists_path, "r") as f:
        cmake_lists = f.readlines()

    base_version = read_base_version(cmake_lists)
    if base_version is None:
        print("ERROR: Could not find base version in CMakeLists.txt", file=sys.stderr)
        sys.exit(1)

    today = date.today().strftime("%Y%m%d")

    # PEP 440 dev version used for PyPI / TestPyPI uploads.
    dev_version = f"{base_version}.dev{today}"
    # CMake does not understand "dev"; use a plain four-component version instead.
    cmake_version = f"{base_version}.{today}"

    print(f"Base version from CMakeLists.txt: {base_version}.")
    print(f"New Python dev version: {dev_version}.")
    print(f"New CMake version: {cmake_version}.")

    print(f"Updating {cmake_lists_path}...")
    counter = 2
    for i, line in enumerate(cmake_lists):
        if CMAKE_KEYWORD in line:
            cmake_lists[i] = CMAKE_KEYWORD + cmake_version + CMAKE_SUFFIX
            counter -= 1
        if EXTENSION_KEYWORD in line:
            cmake_lists[i] = EXTENSION_KEYWORD + EXTENSION_DEV_VERSION + EXTENSION_SUFFIX
            counter -= 1
        if counter == 0:
            break

    with open(cmake_lists_path, "w") as f:
        f.writelines(cmake_lists)

    print("Committing changes...")
    sys.stdout.flush()
    os.system("git config user.email ci@ladybugdb.com")
    os.system('git config user.name "Lbug CI"')
    os.system(f"git add {cmake_lists_path}")
    os.system(
        f'git commit -m "Update CMake version to {cmake_version} and change extension version to dev."'
    )
    sys.stdout.flush()
    sys.stderr.flush()
    print("All done!")
    sys.stdout.flush()


if __name__ == "__main__":
    main()
