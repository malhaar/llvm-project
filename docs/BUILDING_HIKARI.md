# Building Clang with Hikari Obfuscation Passes

Manual instructions for building a clang/clang++ toolchain with Hikari obfuscation baked in.

## Prerequisites

- CMake 3.20+
- Ninja (recommended) or Make
- A working C/C++ compiler (Apple Clang or system GCC)
- ~30 GB disk space for a Release build
- ~8 GB RAM minimum (16 GB recommended)

## 1. Clone the repository

```bash
git clone git@github.com:malhaar/llvm-project.git
cd llvm-project
git checkout hikari-obfuscation
```

> **Note:** The upstream base branch is `apple-arm64e-upstream-next` (Apple's LLVM 19 fork with arm64e/ptrauth support). The `hikari-obfuscation` branch is based on top of that.

## 2. Configure

```bash
cmake -G Ninja -S llvm -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;X86" \
  -DCMAKE_INSTALL_PREFIX=./install \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
```

### Configuration options

| Option | Description |
|--------|-------------|
| `CMAKE_BUILD_TYPE` | `Release` for optimized build, `Debug` for development (much larger and slower) |
| `LLVM_TARGETS_TO_BUILD` | Target architectures. `AArch64;X86` covers Apple Silicon + Intel. Use `all` for everything (slower build) |
| `LLVM_ENABLE_PROJECTS` | Add `"clang;lld"` if you also want the LLD linker |
| `CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER` | Set to `gcc`/`g++` if you don't have clang installed |

## 3. Build

```bash
# Build just clang (fastest)
ninja -C build clang

# Or build everything including tools
ninja -C build

# Parallel jobs (defaults to number of cores; limit if RAM-constrained)
ninja -C build -j4 clang
```

Build time: ~15-45 minutes depending on hardware.

## 4. Verify

```bash
# Check version
build/bin/clang --version

# Test obfuscation passes
echo 'int main() { return 0; }' > /tmp/test.c
build/bin/clang -mllvm -enable-bcfobf -mllvm -enable-cffobf -c /tmp/test.c -o /dev/null
```

Expected output:
```
Running Hikari On /tmp/test.c
Running BogusControlFlow On main
Running ControlFlowFlattening On main
Doing Post-Run Cleanup
Hikari Out
```

## 5. Install (optional)

```bash
ninja -C build install
```

This installs to the path set by `CMAKE_INSTALL_PREFIX` (default: `./install`).

## Available obfuscation flags

All flags are passed via `-mllvm`:

| Flag | Description |
|------|-------------|
| `-enable-bcfobf` | Bogus Control Flow |
| `-enable-cffobf` | Control Flow Flattening |
| `-enable-splitobf` | Basic Block Splitting |
| `-enable-subobf` | Instruction Substitution |
| `-enable-strcry` | String Encryption |
| `-enable-indibran` | Indirect Branching |
| `-enable-funcwra` | Function Call Wrapping |
| `-enable-allobf` | Enable all passes |

### Example

```bash
build/bin/clang -O2 \
  -mllvm -enable-bcfobf \
  -mllvm -enable-cffobf \
  -mllvm -enable-strcry \
  -c myfile.c -o myfile.o
```

### Environment variables

You can also enable passes via environment variables instead of flags:

| Variable | Equivalent flag |
|----------|----------------|
| `BCFOBF=1` | `-enable-bcfobf` |
| `CFFOBF=1` | `-enable-cffobf` |
| `SPLITOBF=1` | `-enable-splitobf` |
| `SUBOBF=1` | `-enable-subobf` |
| `STRCRY=1` | `-enable-strcry` |
| `INDIBRAN=1` | `-enable-indibran` |
| `FUNCWRA=1` | `-enable-funcwra` |
| `ALLOBF=1` | `-enable-allobf` |

## Troubleshooting

### Build fails with linker errors about LLVMObfuscation

Make sure you're on the `hikari-obfuscation` branch. The obfuscation pass sources live in `llvm/lib/Transforms/Obfuscation/`.

### Out of memory during build

Reduce parallelism: `ninja -C build -j2 clang`

### "unknown command line argument" for -enable-* flags

The flag must be passed after `-mllvm`, not directly:
```bash
# Wrong
clang -enable-bcfobf file.c

# Correct
clang -mllvm -enable-bcfobf file.c
```

## What changed (for reference)

If you need to manually wire up the passes on a different branch, these are the 4 files that were modified:

1. **`llvm/lib/Passes/CMakeLists.txt`** — Add `Obfuscation` to `LINK_COMPONENTS`
2. **`llvm/lib/Passes/PassBuilder.cpp`** — Add `#include "llvm/Transforms/Obfuscation/Obfuscation.h"`
3. **`llvm/lib/Passes/PassRegistry.def`** — Add `MODULE_PASS("obfuscation", ObfuscationPass())`
4. **`clang/lib/CodeGen/BackendUtil.cpp`** — Add the include and register the pass:
   ```cpp
   #include "llvm/Transforms/Obfuscation/Obfuscation.h"
   // ...
   PB.registerOptimizerLastEPCallback(
       [](ModulePassManager &MPM, OptimizationLevel Level) {
         MPM.addPass(ObfuscationPass());
       });
   ```
