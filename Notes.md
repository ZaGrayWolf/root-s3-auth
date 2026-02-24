# ROOT S3 Backend Integration Notes

## Build and Environment Setup

**Platform:** macOS ARM64 (Apple Silicon)
**Core Issue:** ROOT's build system silently disables the `net/curl` backend if it cannot definitively locate the `libcurl` dependency. This results in a `libNet.so` that lacks `RCurlConnection` symbols, causing macros to fail at runtime or link time.

**Required Build Configuration:**
A clean, out-of-source build is mandatory to prevent cached LLVM architecture maps from corrupting the configuration on macOS.

```bash
mkdir root-build && cd root-build
cmake ../root \
  -DCMAKE_BUILD_TYPE=Release \
  -Ds3=ON \
  -Dcurl=ON \
  -Dbuiltin_openssl=ON \
  -DCMAKE_PREFIX_PATH=$(brew --prefix curl)

make -j$(sysctl -n hw.ncpu)

```

**Verification:**
To confirm the curl backend compiled successfully into the networking library:
`nm -gU lib/libNet.so | grep RCurlConnection`

## S3 Authentication: HMAC-SHA256 Null-Byte Truncation

**Issue**
AWS S3 requests via ROOT were failing authentication with a `SignatureDoesNotMatch` error. Inspecting the raw HTTP requests showed the `Authorization` header contained signatures much shorter than the required 64 hex characters.

**Root Cause**
The HMAC-SHA256 function generates a 32-byte raw binary hash, which frequently contains null bytes (`0x00`). In `RCurlConnection.cxx`, this binary payload was passed to a `std::string` constructor without an explicit length parameter.

The compiler treated the binary array as a standard null-terminated C-string. Whenever a null byte appeared in the hash, the string was terminated early, and only the truncated portion was hex-encoded. This behavior is especially aggressive on ARM64 architectures.

**Fix**
Modified the string construction to explicitly define the 32-byte length, forcing it to encapsulate the entire binary payload including any null bytes.

```cpp
// Explicitly pass length to prevent null-termination truncation
std::string hash_str(reinterpret_cast<const char*>(raw_hash), 32);

```

## Testing Infrastructure

**Python S3 Mock Server**
Built a local Python mock server to intercept ROOT's `HEAD` and `GET` requests. This allows for direct inspection of the HTTP headers (specifically the `Authorization` signature length) without needing live AWS credentials or dealing with opaque AWS rejection logs.

**Standalone Macro Execution**
Testing network protocol fixes requires isolating the `RCurlConnection` logic from the higher-level `TFile::Open` abstractions.

Wrote a direct C++ macro (`final_proof.C`) to instantiate `ROOT::Internal::RCurlConnection` and manually trigger `SendHeadReq()`.

To run the macro without interpreter path issues:

```cpp
// final_proof.C
#pragma cling add_include_path("../root/net/curl/inc")
#include "ROOT/RCurlConnection.hxx"

void final_proof() {
    gSystem->Load("libNet");
    ROOT::Internal::RCurlConnection conn("http://127.0.0.1:9000/test/verify.root");
    // Inject mock credentials and trigger auth logic...
}

```

Execute in compiled mode to force proper linking against `libNet`:
`./bin/root -l -b -q "final_proof.C+"`

## Ongoing Tasks / Next Steps

* Run full ROOT test suite (`ctest`) against the modified `libNet.so`.
* Verify S3 functionality against a local MinIO instance.
* Prepare patch for upstream submission.
