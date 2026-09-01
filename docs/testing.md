# Testing

What's covered, at what layer, how to run each layer locally, and — since
two of these layers depend on infrastructure this repo doesn't fully
control (a real Docker daemon, a Wokwi account) — what's actually verified
versus best-effort.

## The layers

```
 pure logic, no I/O          real I/O, faked far end        real I/O, real far end
┌──────────────────┐        ┌──────────────────────┐        ┌───────────────────────┐
│ pi-server unit    │        │ integration-tests     │        │ docker-cups-tests      │
│ relay unit         │        │ (real IPP/sync/relay  │        │ (real CUPS daemon)     │
│ firmware host tests│        │  servers, fake `lp`,  │        │                        │
│                    │        │  fake X4 client)      │        │ wokwi-sync-smoke-test  │
│                    │        │                       │        │ (real firmware, real   │
│                    │        │                       │        │  Wi-Fi/HTTPS stack,    │
│                    │        │                       │        │  simulated hardware)   │
└──────────────────┘        └──────────────────────┘        └───────────────────────┘
      fastest, always run           always run                 slower, more real
```

Every layer left of "docker-cups-tests" runs in every CI run
(`.github/workflows/tests.yml`) and locally with nothing beyond Python/
CMake — see README.md's "Running the tests". This document is mostly about
the two layers on the right, which trade speed and reliability for
exercising code paths nothing else in this repo touches.

## pi-server / relay / integration (pytest)

Unchanged from README.md — `pi-server/tests`, `relay/tests`,
`tests/integration`. Notably:

- `printer_forward.py`'s `lp` shell-out is always faked here
  (`fake_lp_binary` fixture) — these suites test the idempotency/dedup
  logic around the call, not that a real CUPS install would accept it.
  `docker-cups-tests` (below) is the one place that gets checked.
- `relay_client.py` (the Pi's poller for the optional cloud relay) is unit
  tested against a fake relay HTTP server in `pi-server/tests/test_relay_client.py`
  — separately from `tests/integration`, which drives the same code through
  a real `RelayServer` instance as part of the full pipeline.
- `config.py`'s admin-console runtime-override layer
  (`save_runtime_overrides`/`load_config`) is tested in `pi-server/tests/test_config.py`
  against a *fresh* `Config` instance reading the settings file back — the
  actual "survives a restart" invariant the module's own docstring
  promises, which `test_admin_api.py`'s HTTP-level round-trip test doesn't
  check (it always reuses the same live `Config` object).

## Firmware host tests (`firmware/test/`)

Unchanged — see `firmware/test/CMakeLists.txt` and
`firmware/.claude/skills/run-firmware/SKILL.md`. These cover the
freestanding, allocation-free logic in `JobStore.h`/`ApprovalOutbox.h`/
`XtcFormat.h` — the split each header documents in its own comment exists
specifically so this logic builds and runs as plain C++17 with no Arduino
toolchain.

Everything on the *other* side of that split — `AtomicJsonFile`/
`JobStore.cpp`/`ApprovalOutbox.cpp`'s SD persistence, `DeviceConfig`/
`WifiStore`'s SD persistence, and all of `net/`/`sync/` (real Wi-Fi, real
HTTPS) — links against `SDCardManager`/`WString`/`WiFi`/`HTTPClient` and
has never been buildable on a plain host. It was previously covered by
nothing at all except manual on-device testing. `wokwi-sync-smoke-test`
(below) is the first automated coverage for the network/sync half of that
gap; the SD-persistence half (`AtomicJsonFile`, the `.cpp` halves of
`JobStore`/`ApprovalOutbox`, `DeviceConfig`, `WifiStore`) is still not
automated anywhere — see "Known gaps" at the end of this document for why.

## Docker: real-CUPS integration (`docker-cups-tests`)

`docker-compose.test.yml` builds three images (`docker/pi-server.Dockerfile`,
`docker/relay.Dockerfile`, `docker/cups/Dockerfile`) for **local dev/
testing only** — this is a separate file from the production
`docker-compose.yml` at the repo root (see `docs/setup-pi.md`'s "Docker
install" section), which reuses the same `docker/pi-server.Dockerfile`
image but talks to a real CUPS already configured on the host instead of
spinning up a throwaway CUPS container. Note for a genuine Pi Zero W:
512MB RAM is already shared with CUPS/avahi/the OS, and Docker's own
overhead is real on top of that — `docs/setup-pi.md` covers a manual
(no-Docker) systemd install as the alternative for that specific case.

`docker/cups/` is a real CUPS daemon with a virtual "PDF" printer
(`printer-driver-cups-pdf`, writes finished jobs to `/output` instead of a
physical device). `tests/docker/test_cups_integration.py` runs *inside*
the `pi-server` container against it, calling `printer_forward.submit_to_cups()`
and `printer_forward.apply_approval()` with the real `lp` CUPS client and
`$CUPS_SERVER` pointed at the `cups` service — the one place in this repo
`lp -d <queue>` (docs/architecture.md "CUPS queue is fixed at install
time") is exercised against an actual CUPS install rather than the fake
`lp` every other suite uses. It asserts the output file is a real,
non-empty PDF (`%PDF-` magic bytes), not just that `lp` exited 0.

Run it locally:

```sh
docker compose -f docker-compose.test.yml build cups pi-server
docker compose -f docker-compose.test.yml up -d cups
# wait for it to report healthy: docker compose -f docker-compose.test.yml ps
docker compose -f docker-compose.test.yml run --rm pi-server python -m pytest tests_docker -q
docker compose -f docker-compose.test.yml down -v
```

CI runs the same sequence as the `docker-cups-tests` job.
`docker/cups/entrypoint.sh` defensively falls back to hand-registering the
"PDF" queue via `lpadmin` if `printer-driver-cups-pdf`'s own postinst
didn't (Debian package behavior can drift across releases); if this job
ever fails at the "cups container never became healthy" step, that
fallback and its PPD auto-detection is the first place to look.

### Known broken: cross-container `lp`/`lpstat` — `continue-on-error: true`

**As of this writing, `docker-cups-tests` does not pass.** Everything
*inside* the `cups` container works correctly and is confirmed by CI on
every run: `cupsd` starts, `cupsctl`/`lpadmin` succeed (both had to be
routed over `-h localhost:631` instead of the default local domain socket
— see `docker/cups/entrypoint.sh`'s header comment for that whole saga),
the "PDF" queue is created, shared (`printer-is-shared=true`), enabled, and
`lpstat -h localhost:631 -p PDF` reports it idle. The healthcheck passes.

What doesn't work: the **`pi-server`** container, submitting cross-container
via `$CUPS_SERVER=cups`, gets `lpstat: Error - add '/version=1.1' to
server name.` on every single CUPS client call — `lpstat -h "$CUPS_SERVER"
-t`, `lpstat -h cups:631 -t`, all of it. DNS resolution is fine
(`getent hosts cups` correctly returns the container's real address). The
obvious reading of that error text — that the client needs an explicit
`/version=1.1` suffix — was tried and **disproved**: setting
`CUPS_SERVER=cups/version=1.1` produced the byte-for-byte identical error,
which means it's CUPS's generic fallback message for some other failure
condition, not an actual missing-version-suffix problem. Something about
this specific cross-container path is different from every local
(`-h localhost:631`, from inside the same container) call this project has
gotten to work reliably.

Seven iterations got this far (see the git history on `docker-compose.test.yml`
and `docker/cups/entrypoint.sh` for the full trail — a kill/restart race, a
`cupsd -f &` job-control quirk, local-domain-socket auth, an unshared
queue, and finally this) before hitting a wall CI's log-only feedback loop
can't get past: **the next step needs a human with real `docker compose`
access** — a packet capture between the containers, or running `cupsd -f`
with verbose IPP logging (`LogLevel debug` in `cupsd.conf`) and watching
what the server actually receives (or doesn't) from the `pi-server`
container's connection attempt. Until then this job is
`continue-on-error: true` in `.github/workflows/tests.yml` so it doesn't
block the rest of CI, but it still runs on every push — if it ever goes
green on its own (a CUPS/Debian package update, say), delete this section
and the `continue-on-error` line together.

## Wokwi: on-device sync smoke test (`wokwi-sync-smoke-test`) — best-effort

**This is the one piece of testing infrastructure in this repo that has
never actually been run end-to-end** (no Wokwi account was available while
building it) — everything below is built from the real file formats/CLIs
involved and Wokwi's documented CI mechanics, not from a green run. Treat
it as a strong starting point to debug from, not a working feature yet.
It's gated (`continue-on-error: true`, and skips entirely without a
`WOKWI_CLI_TOKEN` secret) specifically so it can never block the rest of
CI while that's true.

### What it does

`firmware/test/wokwi_sync/main.cpp` + `platformio.ini`'s
`[env:wokwi_sync_test]` build a stripped firmware image that links the
**real, unmodified** `config/`, `net/`, `store/`, and `sync/` sources —
`WifiManager::connect()`, `SyncClient`'s HTTPS calls, `SyncManager::runFullSync()`
— with no display, buttons, or FreeInkUI. `.github/workflows/tests.yml`'s
`wokwi-sync-smoke-test` job:

1. Builds that firmware with PlatformIO.
2. Starts a real `pi-server` instance (same `driver.sh` the
   `run-pi-server` skill uses) and submits a real IPP print job.
3. Pairs a device against it (`pi-server/tools/pair_device.py` — the exact
   same tool/output format a real deployment uses).
4. Builds a FAT-formatted SD card image (`mtools`, no root/loop devices
   needed) containing `/system/wifi.json` (a static fixture, below),
   `/system/device.json` (from step 3), and `/system/pi_ca.pem` (the
   server's real TLS cert).
5. Runs the firmware in Wokwi's ESP32-C3 simulator
   (`firmware/test/wokwi_sync/diagram.json`) with that SD image attached,
   via `wokwi/wokwi-ci-action`.
6. Passes if the firmware's serial output contains `WOKWI_SYNC_TEST: PASS`
   (printed once `runFullSync()` returns with `wifiConnected == true` and
   zero verification failures) before `fail_text`/the timeout.

`/system/wifi.json` is checked in at `firmware/test/wokwi_sync/fixtures/wifi.json`
with one open network, `Wokwi-GUEST` (Wokwi's documented default simulated
Wi-Fi network) and an empty password — deliberately: `WifiStore`'s saved
password is XOR-obfuscated against the device's own MAC address before
being written, which this repo has no way to precompute for whatever MAC
Wokwi's simulated chip reports; an empty password skips that step
entirely on both the encode and decode side (see `WifiStore.cpp`), so it's
the one credential this harness can commit to disk as a static fixture and
still round-trip correctly regardless of the simulated MAC.

### Setting it up for real

1. Create a free account at wokwi.com and generate a CI token
   (Wokwi's dashboard → CI/CLI tokens).
2. Add it as a repository secret named `WOKWI_CLI_TOKEN`. The job runs
   automatically on the next push once that secret exists — no other
   change needed to turn it on.
3. **Expect to debug it.** The most likely first failure, in order:
   - **`diagram.json`'s pin names.** SD wiring (SCLK=GPIO8, MOSI=GPIO10,
     MISO=GPIO7, CS=GPIO12 — the X4's real, shared-with-display SPI
     pins, see `BoardConfig.h`'s `@@KEEP_FOCUSINK_X4@@` profile) is written from
     reading `BoardConfig.h`, not from ever loading this diagram in
     Wokwi's editor. Open `firmware/test/wokwi_sync/diagram.json` in
     Wokwi's web IDE if the simulator itself fails to start — its editor
     validates part/pin names interactively and is a much faster feedback
     loop than CI.
   - **Reaching the runner's `pi-server` from inside the simulation.**
     `127.0.0.1` from the simulated ESP32's own network stack is *its*
     loopback, not the GitHub Actions runner's. Whether wokwi-cli's local
     network mode makes the runner reachable at `127.0.0.1` anyway, or
     needs a gateway address instead (QEMU/SLIRP-style user-mode networks
     commonly use `10.0.2.2` for "the host" — untested here whether Wokwi
     follows that convention), is unverified. Set a `WOKWI_DEVICE_HOST`
     repository *variable* (Settings → Secrets and variables → Actions →
     Variables) to override once you know the right value — the job
     already reads it (`.github/workflows/tests.yml`, the "Pair a device"
     step) and re-points `pair_device.py --pi-host` at it. If you change
     it away from `127.0.0.1`, also regenerate the pi-server's TLS cert
     with a matching SAN (`pi-server/tools/gen_selfsigned_cert.py --ip
     <value>`) — `SyncClient` does real certificate verification against
     `/system/pi_ca.pem`, so a SAN mismatch fails TLS, not just routing.
   - **`wokwi/wokwi-ci-action`'s exact input names/behavior** may have
     moved since this was written from its documentation rather than a
     real run — check the action's current README against
     `.github/workflows/tests.yml` if the step itself fails to start
     (as opposed to the simulation running and failing).
4. Once it's green, delete this "Setting it up for real" section's
   caveats as they're resolved (or leave them — they're accurate history
   of what had to be debugged, which is useful to the next person who
   touches this).

## Known gaps

Not automated anywhere, deliberately left as a documented gap rather than
worked around with a brittle host shim:

- **SD-backed persistence's actual write path**: `AtomicJsonFile.cpp`
  (the write-then-rename primitive CLAUDE.md's "Atomic durable writes"
  invariant depends on), and the `.cpp` halves of `JobStore`/
  `ApprovalOutbox`/`DeviceConfig`/`WifiStore` that call it. These depend
  on `SDCardManager`/`WString`/`Print` (real Arduino/SdFat types) with no
  portable host equivalent; the in-memory logic these functions serialize
  (`JobIndex`, `ApprovalOutboxIndex`) *is* covered, exhaustively, by
  `firmware/test/`. Building a host-side SdFat/`String` shim faithful
  enough to trust for this specific invariant would be a substantial
  project of its own with real risk of testing the shim's fidelity rather
  than the real behavior; Wokwi's SD card simulation (already wired up
  for `wokwi-sync-smoke-test`, which does exercise `store::saveJobIndex`/
  `saveApprovalOutbox` as a side effect of a real sync pass) is the more
  faithful place this could eventually be strengthened, if a future pass
  adds assertions on the SD image's contents after the simulated run
  rather than only checking the pass/fail serial line.
- **The physical device build itself** (`pio run -e xteink_x4`) — real,
  attempted best-effort in a separate workflow per `tests.yml`'s own
  header comment; needs the `freeink-sdk` submodule and the full ESP32
  toolchain, and is a compile/link check, not a test.
- **FreeInkUI rendering / physical button input** (`ui/InboxUI.cpp`,
  `ui/WebUiServer.cpp`'s HTML/JS surface) — no automated coverage; this is
  the one layer neither host tests nor Wokwi's part library can
  meaningfully stand in for (no SSD1677 e-paper controller model exists
  in Wokwi's parts library, and pixel-level assertions on a simulated
  display wouldn't be a strong signal even if one did).
