# root-s3-auth

This is my submission for the RNTuple S3 Backend evaluation task for GSoC 2026 with CERN/HSF.

## What is in this repo

Two modified files from the ROOT 6.39 source tree:

- `root/net/curl/inc/ROOT/RCurlConnection.hxx`
- `root/net/curl/src/RCurlConnection.cxx`

The changes extend `RCurlConnection` so it can optionally sign HTTP requests with AWS Signature Version 4, which is required for authenticated S3 access.

## What was changed and why

`RCurlConnection` could already send HTTP HEAD and range GET requests via libcurl, but had no way to attach authentication headers. S3 requires every request to carry an `Authorization` header built from a specific HMAC-SHA256 signing chain (AWSv4). Without this, any request to a non-public S3 bucket gets rejected.

The changes add:

- `RS3Credentials` struct to hold the access key, secret key, region, and optional session token
- `SetS3Credentials()` public method to pass credentials into a connection object
- `ApplyS3Auth()` private method that builds the full AWSv4 canonical request, derives the signing key chain, and returns a `curl_slist` with the `Authorization` and `x-amz-date` headers attached
- Hooks in `SendHeadReq()` and `SendRangesReq()` that call `ApplyS3Auth()` when credentials are present and clean up the `curl_slist` immediately after `Perform()` returns

If no credentials are set, nothing changes and the existing behaviour is preserved.

## One thing I ran into

On ARM64 (Apple Silicon), the intermediate HMAC-SHA256 outputs are raw binary and frequently contain null bytes. Passing them to a standard `std::string` constructor without an explicit length caused the string to terminate early at the first null byte, which produced signatures much shorter than the required 64 hex characters. The fix was to always pass the explicit byte length to the constructor so null bytes are treated as data rather than terminators.

## How I tested it

Built ROOT 6.39 from source on macOS ARM64 with `-Dcurl=ON` and `-Ds3=ON`.

Wrote a small Python mock server that listens for incoming HEAD and GET requests and prints the raw headers. Connected to it using a standalone ROOT macro that instantiates `RCurlConnection` directly, injects dummy credentials via `SetS3Credentials()`, and calls `SendHeadReq()`. Used this to verify the Authorization header format and that the signature is consistently 64 hex characters.

Also verified that connections without credentials set do not get any extra headers attached, confirming no regression on standard HTTP requests.

Full testing notes are in `Notes.md`.

## Build environment

- ROOT 6.39, compiled from source
- macOS ARM64 (Apple Silicon)
- OpenSSL via Homebrew
- libcurl with `-Dcurl=ON -Ds3=ON` passed to cmake
