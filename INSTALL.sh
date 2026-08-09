#!/bin/sh
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

# Abort on the first failing command so a broken install cannot report success.
set -eu

# Bazelisk release to install. Checksums are the values published alongside the
# release as bazelisk-linux-<arch>.sha256. Bump all three together.
# These cover the Bazelisk launcher itself. Bazelisk then fetches the Bazel
# release named in .bazelversion, which since v1.29.0 it authenticates with an
# embedded verification key rather than against a hash recorded here.
BAZELISK_VERSION="v1.29.0"
BAZELISK_SHA256_amd64="5a408715e932c0250d28bd84555f12edbf70117de42f9181691c736eacc4a992"
BAZELISK_SHA256_arm64="e20e8b0f4f240091b7a55bf17b9398bd4f40ee70ae0208dff95dd4c445fb4010"
BAZEL_BIN="/usr/local/bin/bazel"

# Run from the checkout root so Bazelisk resolves this repository's
# .bazelversion rather than whatever happens to be in the caller's directory.
unset CDPATH
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

# Containers and cloud-init run this as root, where sudo is usually not
# installed. Everywhere else it is needed to install packages.
if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
else
  SUDO="sudo"
fi

# The Bazel apt repository only publishes amd64 packages, so Bazelisk is used
# instead. Bazelisk reads .bazelversion and fetches that exact Bazel release.
# Bazelisk itself only publishes linux-amd64 and linux-arm64, so those are the
# architectures this script can install; anything else stops with a clear
# message rather than part way through.
ARCH=$(dpkg --print-architecture)
case "$ARCH" in
  amd64) BAZELISK_SHA256="$BAZELISK_SHA256_amd64" ;;
  arm64) BAZELISK_SHA256="$BAZELISK_SHA256_arm64" ;;
  *)
    echo "INSTALL.sh: unsupported architecture '$ARCH' (expected amd64 or arm64)" >&2
    exit 1
    ;;
esac

$SUDO env DEBIAN_FRONTEND=noninteractive apt-get update
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential \
  ca-certificates \
  clang-format \
  curl \
  default-jre-headless \
  protobuf-compiler \
  python3-dev \
  rapidjson-dev \
  unzip \
  zip

# Download as the invoking user, verify against the published checksum, and
# only then install over the system binary. Installing straight from curl would
# leave a truncated executable behind if the transfer were interrupted.
STAGE_DIR=$(mktemp -d)
cleanup() {
  status=$?
  trap - EXIT
  rm -rf "$STAGE_DIR"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

curl -fsSL -o "$STAGE_DIR/bazelisk" \
  "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-${ARCH}"
echo "${BAZELISK_SHA256}  ${STAGE_DIR}/bazelisk" | sha256sum -c -
$SUDO install -m 0755 "$STAGE_DIR/bazelisk" "$BAZEL_BIN"

# Call the installed binary directly; an unrelated bazel earlier in PATH would
# otherwise decide which Bazel the rest of this script uses.
"$BAZEL_BIN" --version

# The pre-push hook is optional; only link it when it is present.
if [ -f "$SCRIPT_DIR/hooks/pre-push" ] && [ -d "$SCRIPT_DIR/.git/hooks" ]; then
  rm -f "$SCRIPT_DIR/.git/hooks/pre-push"
  ln -s "$SCRIPT_DIR/hooks/pre-push" "$SCRIPT_DIR/.git/hooks/pre-push"
fi

# install buildifier
"$BAZEL_BIN" build @com_github_bazelbuild_buildtools//buildifier:buildifier
