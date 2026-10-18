# Updater TLS certificates

The PS5 firmware 8.20 certificate store rejects GitHub's current chain with
`0x8095f00c`; `sceHttpsGetSslError` reports `0x20` (unknown issuer). The release
API and the release-asset CDN need different trust anchors. A successful API
check alone does not establish that the ZIP can download.

The native HTTP context loads `assets/ca-certificates.pem`: curl's 2026-09-25
Mozilla public CA extraction plus the publisher's ISRG Root YR and Root YE.
The original curl bundle's SHA-256 is
`a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`.
It is read through POSIX I/O, from `/app0` or the validated folder location
after shared-data recovery. Server names, certificate chains and validity
checks stay enabled. No verification callback bypasses errors. The host
preview retains libcurl's trust store.

The configured Jellyfin server also chains through the new ISRG hierarchy.
Its endpoint and saved account remain unchanged. Loading the public anchors
fixes both the release API/CDN and the server connection. A TLS failure at
startup must not be mistaken for an empty account.

| Additional CA | SHA-256 fingerprint (DER) | Public source |
| --- | --- | --- |
| ISRG Root YR | `e57b7e6f150c419102e8d5c055729ff967b9d1a829bf00cec89ca604ebf4a86f` | [Root YR](https://letsencrypt.org/certs/gen-y/root-yr.pem) |
| ISRG Root YE | `e14ffcad5b0025731006caa43a121a22d8e9700f4fb9cf852f02a708aa5d5666` | [Root YE](https://letsencrypt.org/certs/gen-y/root-ye.pem) |

These roots are cross-signed by Mozilla-trusted X1/X2. Both cross-signed
certificates were verified against the host public trust store; the bundled
self-signed certificates have identical subjects/public keys. The stock PS5
did not accept the CDN/server chains with X1/X2 alone. Loading YR/YE made
them succeed. Review against the CA publishers when refreshing the bundle.
Do not add server leaf certificates as trust anchors.

## Console evidence, 2026-10-18

The actual `src/web_transport.cpp` adapter was compiled into a diagnostic ELF
and run on firmware 8.20. With the packaged public CA bundle it:

- Read the public release API: HTTP 200, complete 64,272-byte body.
- Followed the public 1.12 ZIP redirect: HTTP 302 then 200.
- Read all 3,694,076 ZIP bytes; SHA-256 matched GitHub's published digest
  `248ef3ff8f2618c5031904f4bf797b55b5de47bd28a47ffa25203c9f4e140546`.
- Rejected `wrong.host.badssl.com` with verification flag `0x04`.
- Rejected `expired.badssl.com` (the bundled anchors do not trust its issuer).

This evidence covers transport, not installation/restart. The configured Jellyfin public endpoint also returned HTTP 200. Host tests
verify CA-load failure, request cleanup and error details.

## Shared-data startup and account persistence

The console launches SlopFin with `/app0` mounted but no `/data` mapping.
Loading settings then returned missing/inaccessible data, so a user could sign
in for the current run without saving it. Startup now loads all font faces
from `/app0`, then calls the existing guarded storage helper, checks that the
shared directory is writable and an existing config is readable, and only
then calls `app::start` / `config::load`. A failed recovery stays on a retry
screen and does not enter account setup. No new mount is created.

On firmware 8.20, the corrected build reached Home after two cold reopens
without signing in. The saved config remained byte-for-byte unchanged against
a private pre-deploy backup; fonts, artwork and the saved profile rendered.
Config saves now fsync the temporary file before replacing the saved file.
The console test build reports 1.12 so it can exercise an upgrade to 1.13.
