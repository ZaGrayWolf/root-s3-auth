# RCurlConnection S3 Extension: Implementation and Testing Notes

## What this does

This extends `RCurlConnection` to attach AWS Signature Version 4 (SigV4) authentication headers to HTTP requests sent via libcurl. Without this, any request to a non-public S3 bucket gets a 403 rejection because the server cannot verify the caller's identity.

The two entry points that needed modification are `SendHeadReq()` and `SendRangesReq()`. Both now call `ApplyS3Auth()` before `Perform()` if credentials have been set, and clean up the resulting `curl_slist` immediately after the transfer completes.

---

## Build and Environment Setup

**Platform:** macOS ARM64 (Apple Silicon)

**Core Issue:** ROOT's build system silently disables the `net/curl` backend if it cannot definitively locate the `libcurl` dependency. This results in a `libNet.so` that lacks `RCurlConnection` symbols, causing macros to fail at runtime or link time.

A clean, out-of-source build is mandatory to prevent cached LLVM architecture maps from corrupting the configuration on macOS:

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

To confirm the curl backend compiled successfully into the networking library:

```bash
nm -gU lib/libNet.so | grep RCurlConnection
```

---

## How AWSv4 Signing Works

The signing process follows the official AWS specification:
- [Authenticating Requests (AWS Signature Version 4)](https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-authenticating-requests.html)
- [Create a signed request](https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_sigv-create-signed-request.html)

There are three stages:

### 1. Build a Canonical Request

The canonical request is a normalized string representation of the HTTP request — method, URI path, query string, headers, signed header names, and a SHA256 hash of the payload. For HEAD and range GET requests the payload is always empty, so the payload hash is always:

```
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
```

Headers must be sorted alphabetically by name. For a GET request with a range header the order is:

```
host:<value>
range:bytes=<value>
x-amz-date:<value>
```

### 2. Derive the Signing Key

The signing key is derived through a chain of four HMAC-SHA256 operations, each building on the previous output:

![AWSv4 Signing Key Derivation](sigv4-signing-key.png)

```cpp
kDate    = HMAC-SHA256("AWS4" + SecretKey, dateStamp)
kRegion  = HMAC-SHA256(kDate,   region)
kService = HMAC-SHA256(kRegion, "s3")
kSigning = HMAC-SHA256(kService, "aws4_request")
```

The intermediate outputs are raw binary and get passed directly into the next HMAC call as keys. Only the final signature is hex-encoded.

### 3. Compute the Final Signature

```
signature = Hex(HMAC-SHA256(kSigning, StringToSign))
```

The `StringToSign` combines the algorithm name, timestamp, credential scope, and a SHA256 hash of the canonical request.

---

## What Was Implemented

### New struct: `RS3Credentials`

Added to `RCurlConnection.hxx` inside `namespace ROOT::Internal`, above the class definition:

```cpp
struct RS3Credentials {
   std::string fAccessKey;
   std::string fSecretKey;
   std::string fRegion;
   std::string fSessionToken; // for temporary IAM role credentials
};
```

### New public method: `SetS3Credentials()`

Added to `RCurlConnection` to allow callers to inject credentials:

```cpp
void SetS3Credentials(const RS3Credentials &creds) {
   fS3Credentials = creds;
   fHasS3Credentials = !fS3Credentials.fAccessKey.empty();
}
```

The `fHasS3Credentials` boolean acts as a fast flag so non-S3 requests pay no cost.

### New private method: `ApplyS3Auth()`

This is the core of the implementation. It builds the full AWSv4 canonical request, derives the signing key chain, constructs the `Authorization` header, and returns a `curl_slist` for the caller to attach and later free:

```cpp
struct curl_slist *ApplyS3Auth(const std::string &method, const char *rangeHeader = nullptr);
```

Three helper functions were added to the anonymous namespace in `RCurlConnection.cxx`:

```cpp
// SHA256 of a string, hex-encoded — used for the canonical request hash
std::string Sha256Hex(const std::string &data);

// Raw binary HMAC-SHA256 — used for intermediate key derivation steps
std::string HmacSha256(const std::string &key, const std::string &data);

// Hex-encoded HMAC-SHA256 — used only for the final signature
std::string HmacSha256Hex(const std::string &key, const std::string &data);
```

### Hooks in `SendHeadReq()` and `SendRangesReq()`

Both methods now follow the same pattern — sign if credentials are present, perform the request, then clean up:

```cpp
struct curl_slist *s3Headers = nullptr;
if (fHasS3Credentials) {
   s3Headers = ApplyS3Auth("HEAD");
   curl_easy_setopt(fHandle, CURLOPT_HTTPHEADER, s3Headers);
}

Perform(status);

if (s3Headers) {
   curl_easy_setopt(fHandle, CURLOPT_HTTPHEADER, nullptr);
   curl_slist_free_all(s3Headers);
}
```

In `SendRangesReq()` this happens inside the batch loop because the `Range` header changes per batch and must be included in the signature for each one individually. A signature computed against one range value will be rejected by S3 if the actual range sent differs.

---

## A Bug Found on ARM64: HMAC Null-Byte Truncation

During testing on macOS ARM64 (Apple Silicon), the mock server was showing signatures much shorter than the expected 64 hex characters, and requests were failing with `SignatureDoesNotMatch`.

The HMAC-SHA256 function generates a 32-byte raw binary hash which frequently contains null bytes (`0x00`). When this binary output was passed between intermediate key derivation steps using a standard C-string constructor, the compiler treated it as a null-terminated string and truncated it at the first null byte. The downstream HMAC operations were then working with a corrupted key.

The fix is to always use the explicit-length constructor so null bytes are treated as data:

```cpp
// Correct — passes explicit byte count, null bytes included
return std::string(reinterpret_cast<char*>(hash), hashLen);
```

After this fix signatures were consistently 64 characters.

---

## Testing

### Python Mock Server

Built a local Python HTTP server to intercept ROOT's `HEAD` and `GET` requests and inspect the raw headers without needing live AWS credentials:

```python
from http.server import HTTPServer, BaseHTTPRequestHandler

class S3Mock(BaseHTTPRequestHandler):
    def do_HEAD(self):
        auth = self.headers.get('Authorization', 'MISSING')
        date = self.headers.get('x-amz-date', 'MISSING')
        print(f"Authorization: {auth}")
        print(f"x-amz-date:    {date}")
        if auth.startswith("AWS4-HMAC-SHA256") and len(auth.split("Signature=")[-1]) == 64:
            print("[PASS] Header format and signature length correct")
        self.send_response(200)
        self.end_headers()

    def do_GET(self):
        auth    = self.headers.get('Authorization', 'MISSING')
        range_h = self.headers.get('range', 'MISSING')
        print(f"Authorization: {auth}")
        print(f"range:         {range_h}")
        if range_h != 'MISSING' and "range" in auth:
            print("[PASS] Range header present and included in SignedHeaders")
        self.send_response(206)
        self.end_headers()

HTTPServer(('127.0.0.1', 9000), S3Mock).serve_forever()
```

### Standalone Macro

Rather than going through `TFile::Open`, wrote a direct C++ macro to instantiate `RCurlConnection` and trigger `SendHeadReq()` in isolation:

```cpp
// final_proof.C
#pragma cling add_include_path("../root/net/curl/inc")
#include "ROOT/RCurlConnection.hxx"

void final_proof() {
   gSystem->Load("libNet");
   ROOT::Internal::RCurlConnection conn("http://127.0.0.1:9000/test/verify.root");
   ROOT::Internal::RS3Credentials creds;
   creds.fAccessKey = "minioadmin";
   creds.fSecretKey = "minioadmin";
   creds.fRegion    = "us-east-1";
   conn.SetS3Credentials(creds);
   uint64_t size = 0;
   conn.SendHeadReq(size);
}
```

Run in compiled mode to force proper linking against `libNet`:

```bash
./bin/root -l -b -q "final_proof.C+"
```

### What Was Verified

- `Authorization` header present and starts with `AWS4-HMAC-SHA256`
- Signature is consistently 64 hex characters
- `x-amz-date` present and correctly formatted
- For GET requests, `range` appears in both the header and in `SignedHeaders` inside the Authorization value
- For HEAD requests, `range` is absent from both
- Connections with no credentials set receive no extra headers — no regression on plain HTTP requests
