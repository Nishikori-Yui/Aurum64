# Development bundle audit

`scripts/package.py` runs a read-only audit of its staged `.app` before replacing
the existing app in `dist`. Packaging fails if the audit fails. The audit can
also check an existing or relocated development bundle without launching it:

```sh
python3 -B scripts/bundle_audit.py dist/Aurum64.app
```

The command requires macOS and the Xcode command-line tools. Success prints a
JSON report with `status: "passed"`; failure exits nonzero and identifies the
failed check. It does not execute the app, load its libraries, start an emulator
session, use the GPU or contact a signing/notarization service.

## Enforced policy

- The local production library list is explicit: Mupen64Plus, CXD4, and the
  Aurum runtime/video/audio/input libraries. `libaurum-*` globbing is not used.
  Leftover diagnostic libraries in a build directory cannot enter the bundle.
- External libraries are restricted to SDL2, libpng and optional SDL3. SDL2-compat
  requires SDL3 even though that dependency is loaded through `dlopen` and is
  absent from the ordinary Mach-O dependency list.
- Only the app executable, Info.plist, approved libraries, Metal library,
  upstream configuration, build manifest, known license notices and code-signing
  resource file are allowed. Unexpected files, empty files, extra directories,
  symlinks, captures, ROMs and private documents fail the audit. New production
  resources or dependency names require an explicit policy change and tests.
- Required upstream and application license notices must be present. SDL2,
  SDL2-compat, SDL3 and libpng notices are checked separately. This checks notice
  presence and integrity, not legal sufficiency or license interpretation.
- Every executable/library must be arm64-only and target macOS. Its deployment
  load command must not exceed the minimum version in Info.plist, which must
  agree with the build manifest. The main executable is checked too, not only
  embedded dylibs.
- The executable must have its bundle-relative Frameworks rpath. Dylib search
  paths, if present, are restricted to `@loader_path`, and install IDs use
  `@rpath/<filename>`. Packaging removes other dylib rpaths, including relative
  Homebrew paths that walk outside the bundle. CXD4 is an `MH_BUNDLE` plugin,
  despite its `.dylib` filename, and must have no install ID. The main binary
  must be `MH_EXECUTE`; the other libraries must be `MH_DYLIB`. Dependencies
  must resolve to an included dylib or a system library under `/usr/lib/` or
  `/System/Library/`. Weak, reexported,
  upward and lazy dependencies are checked as well. Path traversal and external
  absolute dependencies are rejected.
- Each binary's signature and the recursive app signature must verify. An
  ad-hoc signature is acceptable for this development workflow; it is not a
  Developer ID, Gatekeeper, hardened-runtime or notarization assertion.

## Build manifest

The packager writes `Contents/Resources/BuildManifest.json` with
`formatVersion: 1`. Each library records its source hash and its **signed,
bundled** hash. These differ when install names or code signatures change.
Resources and license files have their own hashes; the bundled core patch must
match the build's patch hash. Missing, duplicate or unlisted records fail.

Source labels are repository-relative (`repository:.build/...`) or Homebrew
formula/version paths (`homebrew:sdl3/<version>/lib/...`), not developer home
directories. The root commit, source-snapshot manifest hash or checkout-dirty
status, and production submodule commits remain available for traceability.
The audit never opens the source label as a local path, so it works after
relocation. This change does not strip debug paths from binaries or sanitize
arbitrary source/license text.

The manifest does not hash itself, Info.plist, the main executable or the
code-signing resource seal: those are checked by code-signature verification.
The manifest itself is covered by the app's resource signature. A valid ad-hoc
signature and hashes prove internal consistency, not who produced a bundle.
Use trusted distribution and signing before treating an artifact as authentic.

Old development bundles without the versioned manifest must be rebuilt. There
is no silent fallback that reports them as fully audited.

## Portable tests

```sh
python3 -B -m unittest discover -s Tests -p 'test_bundle_audit.py' -v
python3 -B scripts/bundle_audit.py path/to/Aurum64.app --layout-only
```

The tests use synthetic bundle data and injected command output, so Linux CI
can exercise failure policy without pretending to perform macOS checks.
`--layout-only` checks allowlists, metadata and hashes without Xcode tools. Its
JSON status is `layout-only`, never `passed`, and it does not verify binaries or
signatures. It is not a replacement for the full macOS audit or the existing
dynamic-loader relocation and runtime tests.

Full auditing is wired into normal macOS packaging; the separate `Source
archive` CI workflow runs the portable policy tests. No release, upload,
notarization or compatibility claim is created by either operation.
