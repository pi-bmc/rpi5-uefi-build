---
name: building-firmware
description: Use when building, rebuilding, or compile-checking anything in this repo — after editing an EDK2 source, recipe, patch, or layer file, or when a build is slow, fails to pick up a change, or you need the right bitbake target.
---

# Building this firmware

## Overview

This is a kas-managed Yocto build. **The build command is `kas build`.** Never
invoke `bitbake` directly.

kas reads `.config.yaml` — machine, distro, targets, layers, and the
`local_conf_header` that sets `RPI5_BUILD_TARGET`, `DL_DIR`, `SSTATE_DIR` and
the OP-TEE/StMM knobs. A bare `bitbake` inherits none of it.

## Quick reference

| Goal | Command |
| --- | --- |
| Build everything | `kas build` |
| Same, via the Makefile default | `make` |
| Shell inside the kas environment | `kas shell -c '<cmd>'` |
| One recipe/task, for a fast check | `kas shell -c 'bitbake -c patch edk2'` |
| Formatting gate for EDK2 sources | `make check-format` |

Targets come from `target:` in `.config.yaml`. Do not pass your own target to
`kas build` and do not name a recipe you guessed at — if you think you need a
narrower target, use `kas shell -c 'bitbake ...'` so the kas config still
applies.

## Why not bare bitbake

`bitbake` outside kas misses `local_conf_header` entirely. The most damaging
loss is `RPI5_BUILD_TARGET = "RELEASE"`: a DEBUG build changes which code is
compiled (`DEBUG()` statements, the freed-pool fill), so a "successful" bare
bitbake can validate a binary you will never ship. `DL_DIR` and `SSTATE_DIR`
are also set there, so a bare build silently ignores the shared caches and
refetches from scratch.

You will still SEE `bitbake -c build rpi5-uefi-sdimg rpi5-capsule-image` in
`ps` during a build. That is kas's own child process, not a signal that
someone bypassed kas.

## When the build is unexpectedly slow

Check the sstate summary before assuming you broke something:

```bash
grep "Sstate summary" build/tmp/log/cooker/*/console-latest.log | tail -1
```

A high `Missed` count with a mass rebuild of native recipes (`python3-native`,
`binutils-cross`, `groff-native`) almost always means **the poky layer moved.**

`.config.yaml` pins `poky` to `branch: scarthgap` with no `commit:`, so every
`kas build` fast-forwards it to the current branch tip. When upstream has
landed commits since your last build, the metadata change invalidates sstate
for nearly everything and an incremental build becomes an hour-long one.

Confirm it:

```bash
git -C poky reflog --date=iso -5     # look for a "Reset to" at your build's start time
git diff --submodule=log poky        # what actually moved
```

This is configured behaviour, not a fault. Two consequences worth knowing:

- A `poky` submodule change in `git status` after a build is kas's doing, not
  yours. **Do not commit it** alongside unrelated work — it misattributes an
  upstream bump and makes your own commit unrevertable on its own.
- Builds are not reproducible across days while the branch is unpinned. If
  that matters for a given piece of work, pin `commit:` in `.config.yaml` —
  but that is a deliberate repo-level decision, not something to change in
  passing.

## Verifying a build actually contains your change

`RPI5_BUILD_TARGET = "RELEASE"` means `MDEPKG_NDEBUG` is set and
`BaseDebugLibNull` is linked, so **`DEBUG()` output is compiled out**. A
`strings` grep over the RELEASE binary is not a build check and will mislead
you.

Use instead:

- the `kas build` exit status;
- the recipe's own task logs under `build/tmp/work/*/<recipe>/*/temp/`;
- for an EDK2 patch, confirm the tree really changed:
  `ls build/tmp/work/all-poky-linux/edk2/*/edk2/MdeModulePkg/...`

## Common mistakes

| Mistake | What happens |
| --- | --- |
| `bitbake <target>` directly | No `local_conf_header`: wrong build type, caches ignored |
| Inventing a recipe target | The repo builds `target:` from `.config.yaml`; a guessed recipe may not exist or may not be what ships |
| Committing the `poky` bump with your change | Unrelated upstream commits ride along in your diff |
| Using a `strings`+grep over the binary as proof | RELEASE strips `DEBUG()`; proves nothing either way |
| Assuming a slow build means you broke sstate | Check the poky reflog first — an unpinned branch tip is the usual cause |
