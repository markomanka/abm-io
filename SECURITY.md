# Security policy

## Reporting a vulnerability

Report vulnerabilities privately: **Security → Report a vulnerability**
(https://github.com/markomanka/abm-io/security/advisories/new). Please do not open public issues for
security problems. You will get an acknowledgement within 7 days.

## Scope

The decoders are the main attack surface. `abmio::Unpack` (exchange messages) and `abmio::DecodeDelta`
(codec frames) must reject any malformed input without reading or writing outside the given buffer.
Both are fuzzed on every pull request that touches code and nightly, under AddressSanitizer and UBSan.

## Supported versions

Only `main` receives fixes while the project is pre-1.0.
