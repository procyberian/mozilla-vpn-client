#!/bin/bash

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

set -e


. $(dirname $0)/../../../scripts/utils/commons.sh

RELEASE=1
# Parse Script arguments
while [[ $# -gt 0 ]]; do
  key="$1"
  case $key in
        -d | --debug)
        RELEASE=
        shift
        ;;
    esac
done

# Find the Output Directory and clear that
TASK_HOME=$(dirname "${MOZ_FETCHES_DIR}" )
rm -rf "${TASK_HOME}/artifacts"
mkdir -p "${TASK_HOME}/artifacts"


print N "Taskcluster macOS compilation script"
print N ""


# TC NIT: we need to assert
# that everything is UTF-8
export LC_ALL=en_US.utf-8
export LANG=en_US.utf-8
export PYTHONIOENCODING="UTF-8"

print Y "Installing conda"
source ${TASK_WORKDIR}/fetches/bin/activate
conda-unpack

# Conda Cannot know installed MacOS SDK'S
# and as we use conda'provided clang/llvm
# we need to manually provide the Path.
#
print G "Checking Available SDK'S..."
# Now you would guess the SDK path is the same on all runners
# But no. So... let's find out?
# TODO: Check if this is the same version for every runner on taskcluster .__.
export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)


# Should already have been done by taskcluser, but double checking c:
print Y "Get the submodules..."
git submodule update --init --recursive || die "Failed to init submodules"
print G "done."

# Install dependendy got get-secret.py
python3 -m pip install -r taskcluster/scripts/requirements.txt
print Y "Fetching tokens..."
# Only on a release build we have access to those secrects.
if [[ "$RELEASE" ]]; then
    ./taskcluster/scripts/get-secret.py -s project/mozillavpn/level-1/sentry -k sentry_debug_file_upload_key -f sentry_debug_file_upload_key
else
    echo "dummy" > sentry_debug_file_upload_key
fi

#Install Sentry CLI, if' not already installed from previous run.
if ! command -v sentry-cli &> /dev/null
then
    npm install -g @sentry/cli
fi

# Use vendored crates - if available.
if [ -d ${MOZ_FETCHES_DIR}/cargo-vendor ]; then
mkdir -p .cargo
cat << EOF > .cargo/config.toml
[source.vendored-sources]
directory = "${MOZ_FETCHES_DIR}/cargo-vendor"

[source.crates-io]
replace-with = "vendored-sources"
EOF
fi

print Y "Configuring the build..."
if [ -d ${TASK_HOME}/build ]; then
    echo "Found old build-folder, weird!"
    echo "Removing it..."
    rm -r ${TASK_HOME}/build
fi
mkdir ${TASK_HOME}/build

cmake -S . -B ${TASK_HOME}/build -GNinja \
        -DCMAKE_PREFIX_PATH=${MOZ_FETCHES_DIR}/qt_dist/lib/cmake \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
        -DBUILD_TESTS=OFF

print Y "Building the client..."
cmake --build ${TASK_HOME}/build

print Y "Exporting the build artifacts..."
mkdir -p tmp || die


print Y "Extracting the Symbols..."
dsymutil ${TASK_HOME}/build/src/Mozilla\ VPN.app/Contents/MacOS/Mozilla\ VPN  -o tmp/MozillaVPN.dsym


print Y "Checking & genrating a symbols bundle"
ls tmp/MozillaVPN.dsym/Contents/Resources/DWARF/
sentry-cli difutil check tmp/MozillaVPN.dsym/Contents/Resources/DWARF/*
sentry-cli difutil bundle-sources tmp/MozillaVPN.dsym/Contents/Resources/DWARF/*

if [[ "$RELEASE" ]]; then
    print Y "Uploading the Symbols..."
    sentry-cli login --auth-token $(cat sentry_debug_file_upload_key)
    sentry-cli debug-files upload --org mozilla -p vpn-client tmp/MozillaVPN.dsym/Contents/Resources/DWARF/*
fi

print Y "Compressing the build artifacts..."
tar -C ${TASK_HOME}/build/src/ -czvf ${TASK_HOME}/artifacts/MozillaVPN.tar.gz "Mozilla VPN.app" || die
rm -rf ${TASK_HOME}/build || die

# Check for unintended writes to the source directory.
print G "Ensuring the source dir is clean:"
./scripts/utils/dirtycheck.sh

print G "Done!"
