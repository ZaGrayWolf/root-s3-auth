# RCurlConnection S3 Extension: Implementation and Testing Notes

## What this does

This extends `RCurlConnection` to attach AWS Signature Version 4 (SigV4) authentication headers to HTTP requests sent via libcurl. Without this, any request to a non-public S3 bucket gets a 403 rejection because the server has no way to verify who is making the request.

The two entry points that needed modification are `SendHeadReq()` and `SendRangesReq()`. Both now call `ApplyS3Auth()` before `Perform()` if credentials have been set, and clean up the resulting `curl_slist` immediately after the transfer completes.

---

## Build and Environment Setup

**Platform:** macOS ARM64 (Apple Silicon)

One thing worth noting: ROOT's build system silently disables the `net/curl` backend if it cannot definitively locate the `libcurl` dependency. This results in a `libNet.so` that lacks `RCurlConnection` symbols entirely, which causes macros to fail at runtime with linker errors that are not immediately obvious. A clean out-of-source build with explicit flags avoids this:

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

To confirm the curl backend actually compiled into the networking library:

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

The canonical request is a normalized string of the HTTP request — method, URI path, query string, headers, signed header names, and a SHA256 hash of the payload. For HEAD and range GET requests the payload is always empty, so the payload hash is always:

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

The signing key is not the raw secret — it is derived through a chain of four HMAC-SHA256 operations, each building on the previous output:

![AWSv4 Signing Key Derivation](sigv4-signing-key.png)

```cpp
kDate    = HMAC-SHA256("AWS4" + SecretKey, dateStamp)
kRegion  = HMAC-SHA256(kDate,   region)
kService = HMAC-SHA256(kRegion, "s3")
kSigning = HMAC-SHA256(kService, "aws4_request")
```

The intermediate outputs are raw binary and get passed directly into the next HMAC call. Only the final signature is hex-encoded.

### 3. Compute the Final Signature

```
signature = Hex(HMAC-SHA256(kSigning, StringToSign))
```

The `StringToSign` combines the algorithm name, timestamp, credential scope, and a SHA256 hash of the canonical request.

---

## What Was Changed and Why

### `RCurlConnection.hxx` — credential storage

Added a new `RS3Credentials` struct above the class definition to hold the access key, secret key, region, and an optional session token for temporary IAM credentials:

```cpp
struct RS3Credentials {
   std::string fAccessKey;
   std::string fSecretKey;
   std::string fRegion;
   std::string fSessionToken;
};
```

Added a public `SetS3Credentials()` method so callers can inject credentials into a connection object, and a private `fHasS3Credentials` boolean as a fast flag so non-S3 requests pay zero overhead:

```cpp
void SetS3Credentials(const RS3Credentials &creds) {
   fS3Credentials = creds;
   fHasS3Credentials = !fS3Credentials.fAccessKey.empty();
}
```

Credentials are passed by `const` reference to follow ROOT's convention for complex types and avoid unnecessary copies.

### `RCurlConnection.cxx` — signing logic

Added three helper functions to the existing anonymous namespace:

```cpp
std::string Sha256Hex(const std::string &data);
std::string HmacSha256(const std::string &key, const std::string &data);
std::string HmacSha256Hex(const std::string &key, const std::string &data);
```

The reason there are two HMAC functions is important: intermediate key derivation steps need raw binary output to feed into the next step. Only the very last step needs hex. Using `HmacSha256Hex` for intermediate steps would break the chain.

The `ApplyS3Auth()` method builds the full canonical request, runs the key derivation chain, constructs the `Authorization` header, and returns a `curl_slist`. The caller is responsible for freeing it after `Perform()` returns:

```cpp
struct curl_slist *ApplyS3Auth(const std::string &method, const char *rangeHeader = nullptr);
```

### Hooks in `SendHeadReq()` and `SendRangesReq()`

Both methods follow the same pattern:

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

In `SendRangesReq()` this happens inside the batch loop because the `Range` header changes per batch and must be part of the signature for each one. If you sign against one range value and send a different one, S3 rejects the request.

### Coding conventions

The implementation follows ROOT's coding standards throughout. Member variables use the `f` prefix (`fS3Credentials`, `fHasS3Credentials`) to distinguish them from local variables. The cryptographic code avoids C-style casts in favour of `reinterpret_cast`, uses explicit-length `std::string` constructors to handle binary data safely, and the helpers are scoped inside the anonymous namespace to avoid polluting the global namespace — consistent with how the rest of `RCurlConnection.cxx` is structured.

---

## A Bug Found on ARM64: HMAC Null-Byte Truncation

During testing on macOS ARM64 the signing logic was producing signatures much shorter than the expected 64 hex characters.

The HMAC-SHA256 function generates a 32-byte raw binary hash which often contains null bytes (`0x00`). When this binary output was passed between key derivation steps using a standard C-string constructor, the string terminated at the first null byte. The downstream HMAC calls were then working with a truncated key and producing wrong signatures.

The fix is to always pass the explicit byte length:

```cpp
return std::string(reinterpret_cast<char*>(hash), hashLen);
```

After this, signatures were consistently 64 characters and the signing chain worked correctly on ARM64.

---

## Testing

### HMAC correctness verification

Before touching the ROOT codebase, the three helper functions were extracted into a standalone `.cxx` file and tested against a known input:

```cpp
// Known test: HMAC-SHA256("key", "The quick brown fox jumps over the lazy dog")
std::string result = HmacSha256Hex("key", "The quick brown fox jumps over the lazy dog");
// Expected: f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
```

The output matched exactly, confirming the cryptographic logic was correct before integration.

### Manual header inspection via AWS documentation

The canonical request format, `StringToSign` structure, and `Authorization` header format were verified by tracing through the implementation step by step against the examples in the [AWS SigV4 documentation](https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_sigv-create-signed-request.html). The alphabetical header sorting, the empty payload hash, and the credential scope format were all cross-checked against the spec.

### Python mock server

Wrote a local Python HTTP server to inspect the raw headers that ROOT would send, without needing live AWS credentials:

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

### What was verified

- HMAC helper functions produce correct output against known test vectors
- `Authorization` header starts with `AWS4-HMAC-SHA256` and contains `Credential`, `SignedHeaders`, and `Signature` fields
- Signature is consistently 64 hex characters
- `x-amz-date` is present and correctly formatted
- For GET requests, `range` appears in both the header list and in `SignedHeaders`
- For HEAD requests, `range` is absent from both
- Connections with no credentials set receive no extra headers — no regression on plain HTTP
