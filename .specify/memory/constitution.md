<!--
Sync Impact Report
- Version change: (none) → 1.0.0 (initial ratification)
- Modified principles: n/a (first draft)
- Added sections: Core Principles (5), Performance & Scalability Standards,
  Code Style & Review Process, Governance
- Removed sections: none
- Follow-up TODOs: none
-->
# SCORPIO Constitution

## Core Principles

### I. Portable, Backend-Agnostic I/O API
The public API MUST behave consistently regardless of which low-level I/O backend
(NetCDF, PnetCDF, ADIOS) is active. Backend-specific behavior or types MUST NOT leak
into the public interface. New backend support MUST be added as an internal
implementation, not by branching the public API.
Rationale: SCORPIO's core value is a single portable interface over multiple I/O
libraries; leaking backend details breaks that portability for every downstream
consumer (notably E3SM).

### II. Dual C/Fortran Interface Parity
The C library (`src/clib`) and Fortran library (`src/flib`) MUST expose equivalent
functionality. A change to one interface MUST include the corresponding update to
the other in the same change set, unless explicitly scoped and justified otherwise.
Rationale: SCORPIO is consumed from both C and Fortran model code; divergence
between the two interfaces silently breaks one class of caller.

### III. Test-First CI Gating (NON-NEGOTIABLE)
Every code change MUST add or update tests in `tests/general` or `tests/cunit`
covering the changed behavior. The GitHub Actions CI workflow (SCORPIO + PnetCDF
build and test) MUST pass before a PR is merged. Nightly CDash regression results
MUST be clean before a development-branch change is promoted from `develop` to
`master`.
Rationale: SCORPIO is exascale infrastructure shared by all E3SM components;
regressions are expensive to diagnose downstream, so correctness must be verified
before merge, not after.

### IV. Backward Compatibility for Downstream Consumers
Breaking changes to public API or on-disk format compatibility MUST be explicitly
versioned, documented, and deprecated before removal whenever feasible. Silent
breaking changes to signatures, semantics, or file compatibility are NOT permitted.
Rationale: SCORPIO is the sole I/O layer for E3SM and other structured-grid models;
unannounced breaks stall unrelated science teams' production runs.

### V. Branch & Review Discipline
Development happens on branches named `<github-username>/<feature-description>`
off `master`. Changes merge to `develop` first for nightly testing; promotion from
`develop` to `master` happens only after nightly CI/CDash success, via manual PR
merge. PRs MUST NOT be auto-merged to `master` or `develop`.
Rationale: Nightly, multi-platform validation is the project's actual safety net;
skipping the develop→master gate defeats it.

## Performance & Scalability Standards

Changes affecting data rearrangement, decomposition, or I/O backend calls MUST
consider scaling behavior at high process counts, since SCORPIO targets exascale
Earth system model I/O. Performance-sensitive changes SHOULD be benchmarked against
the existing implementation before merge, and any known scaling regression MUST be
called out explicitly in the PR description.

## Code Style & Review Process

C/C++ sources MUST follow the formatting conventions documented at
https://docs.e3sm.org/scorpio/html/contributing_code.html. All PRs MUST run the
CI workflow defined under `.github/workflows` before being submitted for review.

## Governance

This constitution supersedes ad hoc practice for how changes are made, tested, and
merged in this repository. Amendments are made by editing this file in a PR,
updating the version per the policy below, and stating the rationale in the PR
description.

Versioning policy (semantic versioning applied to this document):
- MAJOR: Backward-incompatible governance changes or removal/redefinition of a
  principle.
- MINOR: A new principle or materially expanded section is added.
- PATCH: Wording clarifications, typo fixes, non-semantic refinements.

All PRs and reviews MUST verify compliance with the principles above. Any
deviation MUST be justified explicitly in the PR description rather than silently
merged.

**Version**: 1.0.0 | **Ratified**: 2026-09-01 | **Last Amended**: 2026-09-01
