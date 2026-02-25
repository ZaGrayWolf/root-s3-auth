# root-s3-auth

This is my submission for the RNTuple S3 Backend evaluation task for GSoC 2026 with CERN/HSF.

## What is in this repo

Two modified files from the ROOT 6.39 source tree:

- `root/net/curl/inc/ROOT/RCurlConnection.hxx`
- `root/net/curl/src/RCurlConnection.cxx`

The changes extend `RCurlConnection` so it can optionally sign HTTP requests with AWS Signature Version 4, which is required for authenticated S3 access.

## What was changed and why

`RCurlConnection` could already send HTTP HEAD and range GET requests via libcurl, but had no way to attach authentication headers. S3 requires every request to carry an `Authorization` header built from a specific HMAC-SHA256 signing chain (AWSv4). Without this, any request to a non-public S3 bucket gets rejected.

To support this, a new `RS3Credentials` struct and a `SetS3Credentials()` method were added so callers can pass in an access key, secret key, and region. A private `ApplyS3Auth()` method handles building the full canonical request and signing key chain, and returns a `curl_slist` that gets attached to the curl handle before each request. Both `SendHeadReq()` and `SendRangesReq()` were updated to call it when credentials are present and clean up the header list immediately after the transfer.

## Error encountered

On ARM64 (Apple Silicon), signatures were coming out much shorter than the expected 64 hex characters. The HMAC-SHA256 output is raw binary and often contains null bytes. Passing it to a `std::string` constructor without an explicit length caused the string to cut off at the first null byte, so the full 32 bytes were not getting through. Fixed by always passing the byte length explicitly to the constructor.

## How I tested it

Wrote a small Python mock server that listens for incoming HEAD and GET requests and prints the raw headers. Used this to verify the Authorization header format and that the signature is consistently 64 hex characters. Also checked that connections without credentials set do not get any extra headers attached, to make sure plain HTTP still works.

Full testing notes are in `Notes.md`.

## Build environment

- ROOT 6.39, compiled from source
- macOS ARM64 (Apple Silicon)
- OpenSSL via Homebrew
- libcurl with `-Dcurl=ON -Ds3=ON` passed to cmake
