# Security

The external scanner (`src/scanner.c`, `src/locals.h`) is C code that runs on any text an editor or tool hands it, so a
crash, an out-of-bounds read or a hang on some input is a security bug.

## Reporting

Report it privately through [GitHub's private vulnerability reporting](https://github.com/Magikdidi24/tree-sitter-ruby/security/advisories/new)
on this repository, with the input that triggers it if you have one. Please don't open a public issue for it.

You should get an answer within a week. Once a fix is released, the advisory is published with credit to you unless you
prefer otherwise.

## Supported versions

Fixes go into the latest release only; update to it.

## What is already checked

- Every pull request: ASan and UBSan on inputs that once crashed, hung or slowed the scanner, a short fuzzing run, and
  CodeQL on the scanner.
- Every night: an hour of fuzzing, and random edits with incremental reparses under ASan and UBSan.
- Packages are published from CI with trusted publishing (OIDC) and npm provenance; no registry token is stored.
