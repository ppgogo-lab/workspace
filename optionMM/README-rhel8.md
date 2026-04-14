# optionMM on Red Hat 8.4

This document describes the dependencies and deployment steps needed to build and run `optionMM` on a fresh Red Hat Enterprise Linux 8.4 host.

## Scope

- Linux server build of `optionmm`
- test builds for `test_simple_mm`, `test_option_mm_core`, and `test_latency`
- runtime requirements for CTP and FEMAS gateway shared libraries

## 1. Required Toolchain

Install the base compiler and build tools first:

```bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y \
  cmake \
  gcc gcc-c++ \
  make \
  pkgconf-pkg-config \
  git
```

Minimum expectations from the repo:

- `cmake >= 3.20`
- C++17-capable `g++`
- `make` or `gmake`

The build requirements come from [CMakeLists.txt](./CMakeLists.txt).

## 2. Required Development Libraries

Install these packages for a normal Linux build:

```bash
sudo dnf install -y \
  protobuf protobuf-devel protobuf-compiler \
  grpc grpc-devel \
  yaml-cpp yaml-cpp-devel \
  spdlog spdlog-devel \
  gtest gtest-devel \
  eigen3-devel \
  gflags-devel \
  glog-devel \
  suitesparse-devel \
  openblas-devel \
  lapack-devel
```

If available in your enabled repositories, also install:

```bash
sudo dnf install -y ceres-solver ceres-solver-devel
```

If `ceres-solver-devel` is not available, build Ceres from source against:

- `Eigen3`
- `gflags`
- `glog`
- BLAS/LAPACK
- SuiteSparse

## 3. Repo-Provided SDK and Runtime Files

The full engine build expects these files in `third_party/`:

### CTP

- `third_party/ctp/ThostFtdcTraderApi.h`
- `third_party/ctp/thosttraderapi_se.so`

### FEMAS

- `third_party/femas/USTPFtdcTraderApi.h`
- `third_party/femas/USTPFtdcMduserApi.h`
- `third_party/femas/libUSTPtraderapiAF.so`
- `third_party/femas/libUSTPmduserapiAF.so`

### OpenSSL compatibility for FEMAS

The FEMAS SDK depends on OpenSSL 1.0-era symbols. This repo already carries compatible runtime files:

- `third_party/compat_ssl/libssl.so.10`
- `third_party/compat_ssl/libcrypto.so.10`

## 4. Runtime Environment

Before running the full binary, export the library search path:

```bash
export LD_LIBRARY_PATH=/path/to/optionMM/third_party/compat_ssl:/path/to/optionMM/third_party/ctp:/path/to/optionMM/third_party/femas:${LD_LIBRARY_PATH}
```

If your host already provides compatible runtime libraries, you can omit some of these paths, but the local `compat_ssl` path is the safest default for FEMAS.

## 5. CPU Requirements

The project currently builds with:

- `-march=native`
- `-mavx2`
- `-mfma`

That means the target machine must support AVX2 and FMA.

Check with:

```bash
lscpu | egrep 'Model name|Flags'
```

If the machine does not support AVX2/FMA, lower the compile flags in [CMakeLists.txt](./CMakeLists.txt) before building.

## 6. Build Modes

### Debug build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
```

Notes:

- `Debug` enables ASAN/UBSAN in this repo
- latency benchmarks are not meaningful in this mode

### Release build

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j"$(nproc)"
```

### RelWithDebInfo build

```bash
cmake -S . -B build-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-relwithdebinfo -j"$(nproc)"
```

Use this for profiling and lower-noise debug work.

## 7. Common Build Targets

### Full binary

```bash
cmake --build build-release --target optionmm -j"$(nproc)"
```

### Core tests

```bash
cmake --build build-release --target \
  test_simple_mm \
  test_option_mm_core \
  test_pre_trade_risk \
  test_latency \
  -j"$(nproc)"
```

### Full gateway library compile check

```bash
cmake --build build-release --target gateway_lib -j"$(nproc)"
```

This is useful to verify CTP/FEMAS integration compiles even if you do not run those gateways locally.

## 8. Validation

Run the core tests:

```bash
cd build-release
ctest --output-on-failure -R 'test_simple_mm|test_option_mm_core|test_pre_trade_risk'
```

Build and run the latency benchmark in a non-ASAN release build:

```bash
cmake -S . -B build-latency-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-latency-release --target test_latency -j"$(nproc)"
./build-latency-release/test_latency --gtest_filter='LatencyTest.TickToQuoteLatency'
```

There is also a convenience script:

```bash
bash scripts/run_latency_release_wsl.sh
```

That script was written for WSL/Linux-style builds and is still useful as a release benchmark entrypoint.

## 9. Configuration Before Running

Before starting `optionmm`, verify:

- the selected YAML config file exists under `config/`
- exchange front addresses are correct
- account IDs, broker IDs, and passwords are populated
- `strategy_type` is set correctly, typically `option_mm_core`
- product underlyings match the instruments you expect from the gateway

## 10. Run

Example:

```bash
export LD_LIBRARY_PATH=/path/to/optionMM/third_party/compat_ssl:/path/to/optionMM/third_party/ctp:/path/to/optionMM/third_party/femas:${LD_LIBRARY_PATH}
./build-release/optionmm config/config.yaml
```

## 11. Minimal Test-Only Build Option

If you only want the simulator-backed test path and do not care about the full exchange gateways at runtime:

- you still need the normal open-source dev packages
- you can build `engine_lib_test`, `test_simple_mm`, `test_option_mm_core`, and `test_latency`
- you do not need to run with CTP/FEMAS credentials

The full top-level `optionmm` binary still links the exchange gateway libraries, so the SDK `.so` files must exist for that target.

## 12. Common Failure Modes

### `Could NOT find Ceres`

Install `ceres-solver-devel` if available, or build Ceres from source.

### `grpc++` not found by pkg-config

Install both gRPC runtime and development packages and confirm:

```bash
pkg-config --libs grpc++
pkg-config --cflags grpc++
```

### FEMAS runtime fails to load SSL symbols

Make sure `LD_LIBRARY_PATH` includes:

- `third_party/compat_ssl`

### Illegal instruction at runtime

The build host and run host likely differ in CPU capabilities. Remove `-march=native` or lower SIMD flags if needed.

### CTP/FEMAS `.so` not found

Confirm these files exist and are readable:

- `third_party/ctp/thosttraderapi_se.so`
- `third_party/femas/libUSTPtraderapiAF.so`
- `third_party/femas/libUSTPmduserapiAF.so`

### Protobuf / gRPC mismatch

Use matching distro packages from the same repository source. Avoid mixing manually built protobuf with distro gRPC unless you control both versions.
