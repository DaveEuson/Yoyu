# Release checklist

How to cut a Yoyu release. Tags drive everything: pushing `vX.Y.Z` builds
the companion apps **and** the Mini firmware image and attaches them to a
GitHub Release; the setup page + companion download links always point at
`releases/latest`.

## What the automation does

| Workflow | Trigger | Produces |
|---|---|---|
| `firmware.yml` | push/PR touching `firmware/**` | compile-check + firmware artifacts (CI gate) |
| `release.yml` | push tag `v*` | `YoyuCompanion-{windows.exe,macos,linux}` + a merged image and a signed OTA `app.bin.sig` **for each board** (`headroom-mini-*`, `yoyu-amoled-*`), attached to the Release |
| `pages.yml` | push to `main` touching `docs/**`, **and after `release.yml` completes** | deploys the setup/flasher page to GitHub Pages, bundling `releases/latest`'s firmware same-origin |

`pages.yml` is the only workflow that deploys Pages. It also refuses to publish
a flasher whose firmware doesn't match the current release — v1.5.0 shipped with
the page serving the previous version because two workflows deployed the site
from two different sources and the stale one landed last, with both runs green.

Fixed URLs the site depends on (resolve once a Release exists):
- Flasher images, per board: `.../releases/latest/download/{headroom-mini,yoyu-amoled}-{bootloader,partitions,boot_app0,app}.bin`
  — the setup page's board picker chooses between `docs/firmware/manifest.json`
  and `manifest-amoled216.json`, and `pages.yml` bundles **both** sets. A board
  the picker can offer whose parts were never bundled 404s at Install time,
  which looks like a broken board rather than a broken deploy.
- Companion: `.../releases/latest/download/YoyuCompanion-{windows.exe,macos,linux}`
- Setup page: `https://daveeuson.github.io/Yoyu/`

## One-time setup (first release only)

- [ ] **GitHub → Settings → Pages → Source = "GitHub Actions."** Without this,
      `pages.yml` has nothing to publish to and the flasher page never goes live.
- [ ] **Add the `OTA_SIGNING_KEY` repo secret** (the EC P-256 private key —
      keygen + steps in `docs/HARDENING.md`). Signature-checking firmware
      refuses an unsigned OTA image, and `release.yml` hard-fails without this
      secret. Required from the first signed-OTA release onward.

## Every release

1. [ ] **Bump the version.** Firmware `FW_VERSION` + `UA` in
       `firmware/src/main.cpp`; companion `USER_AGENT` in
       `companion/companion.py` if it changed. Keep them in step with the tag.
2. [ ] **Write the notes** into `docs/release-notes/vX.Y.Z.md`. Every release
       through v1.3.1 published with an **empty body**, because this checklist
       never said to write one and the notes only ever existed in the terminal
       history of whoever cut the release. Keeping them in the repo makes them
       reviewable in the diff and gives step 7 a stable file to point at.
3. [ ] **Green CI on the branch** — `firmware.yml` (the only pre-tag compile
       check for the firmware) and `companion.yml` (unit tests) must be passing.
4. [ ] **Merge the PR into `main`.** This fires `pages.yml`, which redeploys the
       setup page. (It does *not* build binaries — only the tag does.)
5. [ ] **Create the release / tag.** The tag **must start with `v`** (e.g.
       `v1.0.0`) — `release.yml` only triggers on `v*`, so a tag like `1.4.0`
       silently builds nothing.

       **Tag `origin/main` explicitly, never the working copy.** Tagging
       whatever your local checkout happens to be on has shipped the wrong
       commit twice: once a months-old commit, once one commit short of the
       intended content. `release.yml` now cross-checks the tag against
       `FW_VERSION` and fails fast on a mismatch, but the habit is the real fix:
       ```
       git fetch origin main
       git tag v1.0.0 origin/main
       git push origin v1.0.0
       ```
       or on GitHub: **Releases → Draft a new release → Choose a tag → type
       `v1.0.0` → Create new tag on publish → Publish**. Either way `release.yml`
       builds the three companion apps + the merged firmware image and attaches
       them to Release `v1.0.0`.

       *Re-pointing a tag* (only safe while no release has been published for
       it): `git tag -d v1.0.0 && git push origin :refs/tags/v1.0.0`, then
       re-create and push as above.
6. [ ] **Watch `release.yml` go green** and confirm the Release has the three
       `YoyuCompanion-*` apps plus the firmware images
       (`headroom-mini-bootloader/partitions/boot_app0/app.bin` for the flasher,
       and `headroom-mini-merged.bin` for esptool users).
7. [ ] **Apply the notes.** `release.yml` publishes the release with an empty
       body — `softprops/action-gh-release` is given files, not a body — so the
       notes go on afterwards:
       ```
       gh release edit v1.0.0 --notes-file docs/release-notes/v1.0.0.md
       ```
8. [ ] **Smoke test the retail path** in Chrome/Edge:
       - Open `https://daveeuson.github.io/Yoyu/`, click **Connect &
         Install**, flash a board.
       - Same window → **Connect to Wi-Fi** (Improv) → board joins.
       - Open `http://<board-ip>:8080` → styled landing page loads.
       - Download a companion binary from the page and confirm it feeds the
         board (`--pi`, or auto-discovered).
       - **Pair (self-contained):** `YoyuCompanion --pair` (auto-finds the
         board) → status flips to "Running self-contained" and meters update
         with no companion running. (`/connect` manual paste is the fallback.)
       - `/alerts` → set an ntfy topic → **Send test alert** lands on a phone.
       - **OTA:** from a board on the previous release, open `/update` and
         confirm it installs the new signed image and reboots on the new
         version (the signature is accepted). Check the version without
         standing over the board — and for more than one at a time — with
         `curl -s http://<board-ip>:8080/api/status`, which reports `version`,
         `board`, `poll_status` and `release_check`.
       - **Exercise the change itself, against the published artifact.** Not
         the branch, not the source — the file someone downloading right now
         would get. See below.

### Test the artifact, not the branch

A green test run says the source is right. It says nothing about whether the
code path a user actually takes reaches it, and that is a different question.

v1.6.1 shipped a companion fix that cleared stale auto-start entries. Its tests
passed, and the fix was real. It also only worked in `YoyuCompanion-cli`:
`tray.py` has its own entry point that never reached the new code, and the tray
app is the download the setup page points everyone at. So the users most likely
to hit the bug were exactly the ones the fix missed, and v1.6.2 existed only to
correct that. Downloading both binaries and running them would have caught it
in two minutes; no amount of testing the branch ever would.

So for whatever the release actually changes:

- [ ] **Download the published asset** for it — the real one, from the release
      page, not a local build.
- [ ] **Reproduce the condition it fixes**, then run it. If the fix is "removes
      a stale file", plant the stale file. Plant something similar it must
      *not* touch, too — a fix that over-reaches is worse than the bug.
- [ ] **Run every build that ships the change.** There are two companion
      binaries, and they have separate entry points.

Two things that look like failures and are not, both met while doing exactly
this:

- **Searching a PyInstaller `.exe` for a function name finds nothing.** The
  bytecode is compressed inside the binary; string search cannot see it. Test
  the behaviour, not the bytes.
- **Piped stdout from the CLI can come back empty**, including its normal
  banner. It is block-buffered, and killing the process discards the buffer. A
  real terminal is line-buffered and shows it. Redirect to a file and let the
  process exit, or read stderr, before concluding nothing was printed.

## Release notes template

```
## Yoyu v1.0.0

### Yoyu (ESP32-S3) — first full firmware
- Browser flasher (ESP Web Tools) + Wi-Fi over USB (Improv) — no VS Code/CLI.
- Self-contained on-device usage polling — pair it once with the companion
  (`--pair`), no computer needed afterward; manual `/connect` paste as fallback.
- Touch (cycle screens, % used/left, brightness) + motion (flip to sleep,
  shake wake), battery gauge, usage-history graph, phone push alerts (/alerts).

### Companion
- `--pair` hands a board your login so it runs self-contained.
- Multi-device push (comma-separated --pi), single-instance lock, live-usage
  backoff.
```

## Signed firmware updates (OTA)

From the first signature-checking build onward, the board verifies an ECDSA
P-256 signature before applying any OTA image (details in `docs/HARDENING.md`).
Two things to keep straight:

- **Every release must be signed.** `release.yml` signs `headroom-mini-app.bin`
  with the `OTA_SIGNING_KEY` secret and publishes `headroom-mini-app.bin.sig`
  next to it. The signing step hard-fails if the secret is missing, so you can't
  accidentally ship a release that signature-checking boards will reject.
- **Bootstrap order (matters once).** A board already running signed-OTA
  firmware only accepts signed releases. The *first* signed build therefore has
  to reach a board another way — an OTA from the previous, non-checking firmware,
  or a USB re-flash. After that, every OTA is verified. A signed build will not
  downgrade to a pre-signing release (those carry no `.sig`).

**Rotating the signing key:** generate a new EC P-256 keypair, replace the PEM
in `firmware/src/ota_pubkey.h`, update the `OTA_SIGNING_KEY` secret, and ship
that build — again, its first delivery is OTA-from-old or USB, since existing
boards trust the old key until they run the new firmware.

## Rollback

Releases are immutable; to ship a fix, tag a new patch (`v1.0.1`). The setup
page and companion links track `releases/latest`, so a new Release moves users
forward automatically — nothing else to update. (A signed-OTA board only rolls
forward to another *signed* release, not back to a pre-signing one.)
