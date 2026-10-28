# Source snapshots

`scripts/source_archive.py` exports a committed revision and its pinned
submodules into one `.tar.gz`. It needs Python 3.9+ and Git 2.31+, not Xcode,
Metal, Homebrew libraries, a running emulator, or network access.

This is a development source snapshot, **not a release-ready corresponding
source package**. Homebrew dependency sources are still unpinned and absent.
The existing build scripts also depend on Git metadata, which this archive
deliberately does not include. Building directly from an extracted snapshot
without Git needs separate build-script work. See [dependency provenance](DEPENDENCIES.md)
for licensing and release requirements.

## Generate locally

First initialize pinned submodules in a checkout where nobody else is working:

```sh
git submodule update --init --recursive
```

The export itself never initializes, updates, fetches, resets or writes a
submodule. Partial/promisor clones are rejected to prevent implicit object
fetches. A linked worktree can use submodule objects already available in the
main checkout; its submodule directories can remain empty. Keep a shared Git
object database available for the duration of the export (do not concurrently
prune required objects).

```sh
python3 scripts/source_archive.py --ref HEAD --output dist/Aurum64-source.tar.gz
# Compare independent exports of the same resolved commit:
python3 scripts/source_archive.py --ref HEAD --output dist/Aurum64-source-again.tar.gz
cmp dist/Aurum64-source.tar.gz dist/Aurum64-source-again.tar.gz
```

Choose an exact commit instead of `HEAD` if another process may move the branch
between commands. `--repo /path/to/checkout` selects a different local repository.
Only committed content is exported: commit intended changes before generating a
snapshot that should contain them. The tool prints the finished archive's SHA-256
and path. It refuses to overwrite existing files and does not publish anything.

## Contents and checks

- The top-level directory is `Aurum64-<12-character-commit>-source`.
- Files come directly from Git blobs, with executable bits and safe relative
  symlinks preserved. Checkout edits, staged changes and untracked/ignored files
  are not read. Local Git replacement objects are ignored.
- Submodules, including nested ones, come from the parent commit's gitlinks,
  not their currently checked-out `HEAD`. Missing repositories, mappings or
  pinned objects fail the export; dependencies are never silently omitted.
- `SOURCE_MANIFEST.json` records the full root and submodule commits, each
  file's Git object ID, mode and SHA-256. It does not add local source paths,
  remote URLs, account names, machine names or wall-clock timestamps.
- Entries are sorted; archive ownership and gzip headers are normalized. Tar
  timestamps use the root commit timestamp. Repeated exports are byte-identical
  with the same Python/zlib toolchain. Compression across toolchain versions is
  not promised to be byte-identical.
- `.gitattributes` export filters are not applied, so `export-ignore` cannot
  accidentally remove a license or source file. Upstream files and notices are
  retained unchanged; the local core patch stays in `patches/` and is not applied
  to the archived upstream source.

Known private/generated paths such as top-level `PrivateDocs`, `.build`, `dist`,
ROM/save/capture files and `.env` files are rejected if tracked. The top-level
`SOURCE_MANIFEST.json` name is reserved for the generated manifest, including
directory and case variants. File and implied directory paths that collide
under case folding or Unicode normalization are rejected, so they cannot
overwrite one another on default macOS volumes. Unsafe paths or symlinks also
fail the export, and failed runs leave no partial destination.
These filename checks are not a general secret scanner: review the selected
commit before sharing. The manifest's lack of local paths does not sanitize
paths, URLs or secrets someone has committed inside source files.

All pinned submodules are included, including the development-only Angrylion
reference and its upstream license. Its distribution restrictions still apply;
this tool does not turn that reference into a production app component or grant
permission to redistribute it commercially.

## Verification

```sh
python3 -B -m unittest discover -s Tests -p 'test_source_archive.py' -v
```

Tests create disposable local Git repositories with synthetic data. They cover
dirty checkouts and indexes, advanced submodule checkouts, recursive gitlinks,
linked worktrees, missing dependencies, hashes, file modes, symlinks, privacy
guards, macOS path collisions, extraction round trips, failure cleanup and
concurrent output protection. No existing checkout, build directory, ROM, app
process or GPU is used.

The separate `Source archive` workflow runs these tests and compares two real
repository exports on Linux using Python 3.9, 3.12 and 3.14. Each version checks
its own repeated output; the workflow does not require identical compressed
bytes across Python/zlib versions. It does not alter the macOS build workflow,
upload archives or create a release.
