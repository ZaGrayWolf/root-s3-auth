# root-s3-auth

This is my submission for the RNTuple S3 Backend evaluation task for GSoC 2026 with CERN/HSF.

## What is in this repo

Two modified files from the ROOT 6.39 source tree:

- `root/net/curl/inc/ROOT/RCurlConnection.hxx`
- `root/net/curl/src/RCurlConnection.cxx`

Also included is `minio_test.cxx`, a standalone libcurl test used for integration testing against a local MinIO server.

The changes extend `RCurlConnection` so it can optionally sign HTTP requests with AWS Signature Version 4, which is required for authenticated S3 access.

## What was changed and why

`RCurlConnection` could already send HTTP HEAD and range GET requests via libcurl, but had no way to attach authentication headers. S3 requires every request to carry an `Authorization` header built from an AWS Signature Version 4 signing chain. Without this, any request to a non-public S3 bucket gets rejected.

To support this, a new `RS3Credentials` struct and a `SetS3Credentials()` method were added so callers can pass in an access key, secret key, and region. Rather than implementing the signing chain manually, the implementation uses libcurl's built-in `CURLOPT_AWS_SIGV4` option introduced in libcurl 7.75.0, credentials go in via `CURLOPT_USERPWD`, and libcurl handles the full canonical request, key derivation, and signature internally. Both `SendHeadReq()` and `SendRangesReq()` were updated to set these options when credentials are present, wrapped in a `HAS_CURL_AWS_SIGV4` version guard so the code still compiles on older libcurl versions.

## How I tested it

**Integration test:** Set up a local MinIO instance and ran `minio_test.cxx` against it, a standalone libcurl program that sets `CURLOPT_AWS_SIGV4` with MinIO credentials and hits a private bucket. MinIO returned HTTP 200, confirming the signing works end to end against a real S3 compatible server.

**Unit tests:** Before that, wrote a small Python mock server that intercepts HEAD and GET requests and prints the raw headers. Used this to verify the `Authorization` header format, that signatures are consistently 64 hex characters, and that `range` shows up in `SignedHeaders` for GET requests. Also confirmed that connections without credentials set get no extra headers attached, to make sure plain HTTP still works.

Full testing notes are in `Notes.md`.

## Build environment

- ROOT 6.39 source tree
- macOS ARM64 (Apple Silicon)
- libcurl 8.7.1 (ships with macOS 15, supports `CURLOPT_AWS_SIGV4`)
- MinIO for integration testing
