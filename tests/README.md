# The wire contract with server-rs

Everything the driver and server-rs must agree on lives in one place,
`third_party/schemas/contract.json` (see that repo's README): routes, the
`path` query key, status codes, size limits, the exact request bytes the
driver sends, golden FlatBuffer fixtures, and the *behaviours* each side
assumes of the other (B01-B11: Content-Length framing, keep-alive, exact
206 ranges, the error-to-NTSTATUS table, Windows-style paths, and so on).

- `src/Client.c` builds its request lines and expected statuses from the
  generated `blorg_contract.h`, and `C_ASSERT`s its body and header ceilings
  against it. Don't reintroduce a literal route, status or limit.
- `tests/sandbox/ContractTest.cpp` sends the contract's requests and checks
  the bytes on the wire, decodes the flatc-encoded fixtures through the real
  receive path, and checks the status table. Every test is tagged
  `contract: Bnn`. `contract.yml` fails if a behaviour the driver is
  responsible for has no tagged sandbox test.
- `tests/guest-suites/Contract.ps1` reads the contract corpus back through
  the mounted drive and runs the live wire probe against the server. The
  corpus, `tests/guest-suites/Contract.corpus/`, is exactly the tree the
  probe seeds; the test host serves it as `Contract\` and lists it in
  `-CorpusManifest`, and `contract.yml` fails if it drifts from the pinned
  probe. Regenerate it with:

  ```powershell
  third_party\schemas\conformance\Test-BlorgContract.ps1 -Server unused:0 `
      -SeedRoot <dir> -ProbeDir Contract.corpus -SeedOnly
  ```

  The suite can't change a file on the server's host, so the checks that
  need one (B08 after a change, B11) run in server-rs CI, where the probe
  gets `-SeedRoot`. The two known gaps (B09 case sensitivity against a
  Linux host, B11 a read across a file that shrank under a cached size)
  are reported as INFO.

## Running it by hand

Against any running server-rs whose served directory holds a copy of
`Contract.corpus` as `Contract\`, with the driver mounted on `B:`:

```powershell
tests\guest-suites\Contract.ps1 -Drive B: -BackendUrl http://<server>:<port>
```

Without `-CorpusManifest` the suite compares the drive against the
committed corpus. The exit code is the number of failed checks.

## Changing the contract

Changing what the driver sends or expects is a contract change: edit
`contract.json` in Chuccle/schemas first, then server-rs, then move this
repo's `third_party/schemas` and `third_party/server-rs` pins together in
one commit (the package pins check fails otherwise).
