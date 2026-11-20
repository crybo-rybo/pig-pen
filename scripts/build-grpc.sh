#!/usr/bin/env bash
# Build the pinned gRPC (with its bundled Protobuf, Abseil, and RE2) with the
# project compiler, and install it into a prefix that Pig Pen can use.
set -euo pipefail

GRPC_VERSION="v1.84.0"
GRPC_COMMIT="3252a89f10d8e92997862167ca7d095ecda85973"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
default_prefix="${repo_root}/build/deps/grpc"
work_dir="${repo_root}/build/deps/_grpc-work"
stamp_name="pigpen-grpc.stamp"

usage() {
  cat <<EOF
Usage: scripts/build-grpc.sh [--force] [PREFIX]

Build gRPC ${GRPC_VERSION} and the Protobuf, Abseil, and RE2 versions that
it bundles, with the same compiler and C++ standard library as Pig Pen.
Install them as static libraries into PREFIX. The script checks that the
tag is at the pinned commit ${GRPC_COMMIT}.

PREFIX defaults to build/deps/grpc in the repository. The Pig Pen configure
step uses that prefix automatically. For a different prefix, give
-DPIGPEN_GRPC_ROOT=PREFIX to the configure step.

Options:
  --force     Build and install again, also when the prefix is up to date.
  -h, --help  Show this text.

Environment:
  CC, CXX     The compilers. The defaults are gcc-16 and g++-16 on macOS,
              and gcc and g++ on other systems. CXX must be GCC 16.2 or later.
  CMAKE_BUILD_PARALLEL_LEVEL
              The number of parallel build jobs. The default is the number
              of cores.
  KEEP_WORK=1 Keep the work directory (build/deps/_grpc-work) after a
              successful install.

Necessary tools: git, CMake 3.31 or later, and Ninja.

On macOS, OpenSSL and c-ares come from Homebrew:
  brew install openssl@3 c-ares

On Linux, OpenSSL, zlib, and c-ares come from the system packages. On
Debian and Ubuntu, install them with:
  sudo apt-get install libssl-dev zlib1g-dev libc-ares-dev

The build takes about ten minutes on 12 cores. The installed prefix is
about 120 MB.
EOF
}

die() {
  echo "build-grpc.sh: error: $*" >&2
  exit 1
}

force=0
prefix=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h | --help)
      usage
      exit 0
      ;;
    --force)
      force=1
      ;;
    -*)
      die "unknown option '$1' (see --help)"
      ;;
    *)
      [[ -z "${prefix}" ]] || die "give at most one PREFIX (see --help)"
      prefix="$1"
      ;;
  esac
  shift
done
prefix="${prefix:-${default_prefix}}"
[[ "${prefix}" == /* ]] || prefix="${PWD}/${prefix}"

os="$(uname -s)"
if [[ "${os}" == "Darwin" ]]; then
  cc="${CC:-gcc-16}"
  cxx="${CXX:-g++-16}"
else
  cc="${CC:-gcc}"
  cxx="${CXX:-g++}"
fi

for tool in git cmake ninja; do
  command -v "${tool}" >/dev/null || die "'${tool}' is not installed"
done
command -v "${cc}" >/dev/null || die "the C compiler '${cc}' is not installed"
command -v "${cxx}" >/dev/null ||
  die "the C++ compiler '${cxx}' is not installed (set CXX to a GCC 16.2 or later)"

# Pig Pen needs GCC 16.2 or later. The gRPC libraries must use the same C++
# standard library, so the same compiler must build them.
macros="$("${cxx}" -dM -E -x c++ /dev/null)"
gnu_major="$(awk '$2 == "__GNUC__" { print $3 }' <<<"${macros}")"
gnu_minor="$(awk '$2 == "__GNUC_MINOR__" { print $3 }' <<<"${macros}")"
if grep -q '__clang__' <<<"${macros}" || [[ -z "${gnu_major}" ]] ||
  ((gnu_major < 16 || (gnu_major == 16 && gnu_minor < 2))); then
  die "'${cxx}' is not GCC 16.2 or later ($("${cxx}" --version | head -n 1)).
Set CXX (and CC) to the compiler that builds Pig Pen, for example
CC=gcc-16 CXX=g++-16."
fi

cc_version="$("${cc}" --version | head -n 1)"
cxx_version="$("${cxx}" --version | head -n 1)"
stamp_file="${prefix}/${stamp_name}"
stamp="grpc_version=${GRPC_VERSION}
grpc_commit=${GRPC_COMMIT}
cc=${cc_version}
cxx=${cxx_version}"

configure_hint() {
  local compilers=""
  if [[ "${os}" == "Darwin" ]]; then
    compilers="CC=/usr/bin/cc CXX=${cxx} "
  fi
  echo "gRPC prefix: ${prefix}"
  echo "Configure Pig Pen with:"
  echo "  ${compilers}cmake --preset dev --fresh -DPIGPEN_GRPC_ROOT=${prefix}"
}

if [[ "${force}" -eq 0 && -f "${stamp_file}" ]] &&
  [[ "$(cat "${stamp_file}")" == "${stamp}" ]]; then
  echo "The gRPC prefix is up to date (gRPC ${GRPC_VERSION}, ${cxx_version})."
  configure_hint
  exit 0
fi

platform_args=()
if [[ "${os}" == "Darwin" ]]; then
  command -v brew >/dev/null ||
    die "Homebrew is not installed. Install it, then: brew install openssl@3 c-ares"
  openssl_root="$(brew --prefix openssl@3 2>/dev/null || true)"
  cares_root="$(brew --prefix c-ares 2>/dev/null || true)"
  [[ -n "${openssl_root}" && -f "${openssl_root}/include/openssl/ssl.h" ]] ||
    die "the Homebrew OpenSSL is missing. Install it: brew install openssl@3 c-ares"
  [[ -n "${cares_root}" && -f "${cares_root}/include/ares.h" ]] ||
    die "the Homebrew c-ares is missing. Install it: brew install openssl@3 c-ares"
  # Abseil's time_zone_format.cc defines _XOPEN_SOURCE. On macOS that hides
  # declarations that libstdc++'s <cstdlib> needs (at_quick_exit).
  platform_args+=(
    "-DCMAKE_C_FLAGS=-D_DARWIN_C_SOURCE"
    "-DCMAKE_CXX_FLAGS=-D_DARWIN_C_SOURCE"
    "-DOPENSSL_ROOT_DIR=${openssl_root}"
    "-DCMAKE_PREFIX_PATH=${cares_root}"
  )
fi

jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
src_dir="${work_dir}/grpc"
build_dir="${work_dir}/build"

echo "Building gRPC ${GRPC_VERSION} with ${cxx_version}"
echo "  prefix: ${prefix}"
echo "  work:   ${work_dir}"
echo "  jobs:   ${jobs}"

# Use a clone from an earlier, failed run again only when it is at the pin.
if [[ -d "${src_dir}/.git" ]] &&
  [[ "$(git -C "${src_dir}" rev-parse HEAD 2>/dev/null || true)" == "${GRPC_COMMIT}" ]]; then
  echo "Using the existing clone in ${src_dir}"
else
  rm -rf "${src_dir}"
  mkdir -p "${work_dir}"
  git -c advice.detachedHead=false clone --depth 1 --branch "${GRPC_VERSION}" \
    https://github.com/grpc/grpc.git "${src_dir}"
fi
head="$(git -C "${src_dir}" rev-parse HEAD)"
[[ "${head}" == "${GRPC_COMMIT}" ]] ||
  die "the tag ${GRPC_VERSION} is at commit ${head}, not at the pinned ${GRPC_COMMIT}"

# The gRPC configure step needs third_party/grpc-proto.
git -C "${src_dir}" submodule update --init --depth 1 \
  third_party/abseil-cpp third_party/protobuf third_party/re2 \
  third_party/grpc-proto

cmake -S "${src_dir}" -B "${build_dir}" -G Ninja --fresh \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="${cc}" \
  -DCMAKE_CXX_COMPILER="${cxx}" \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_INSTALL_PREFIX="${prefix}" \
  -DBUILD_SHARED_LIBS=OFF \
  -DgRPC_INSTALL=ON \
  -DgRPC_BUILD_TESTS=OFF \
  -DABSL_PROPAGATE_CXX_STD=ON \
  -Dprotobuf_BUILD_TESTS=OFF \
  -DgRPC_ABSL_PROVIDER=module \
  -DgRPC_PROTOBUF_PROVIDER=module \
  -DgRPC_RE2_PROVIDER=module \
  -DgRPC_SSL_PROVIDER=package \
  -DgRPC_ZLIB_PROVIDER=package \
  -DgRPC_CARES_PROVIDER=package \
  -DgRPC_BUILD_CSHARP_EXT=OFF \
  -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
  ${platform_args[@]+"${platform_args[@]}"}
cmake --build "${build_dir}" --parallel "${jobs}"

# Start the default prefix from empty, so that no files of an earlier version
# stay. A prefix that the caller names is never deleted.
if [[ "${prefix}" == "${default_prefix}" ]]; then
  rm -rf "${prefix}"
fi
rm -f "${stamp_file}"
cmake --install "${build_dir}"
printf '%s\n' "${stamp}" >"${stamp_file}"

if [[ "${KEEP_WORK:-0}" == "1" ]]; then
  echo "Kept the work directory ${work_dir}"
else
  rm -rf "${work_dir}"
fi

echo "Installed gRPC ${GRPC_VERSION}."
configure_hint
