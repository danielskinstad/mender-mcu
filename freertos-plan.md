# FreeRTOS port of mender-mcu — plan

*2026-07-06. Synthesis of `freertos-port-research-brief.md`, `freertos-esp-idf-mender-primer.md`, and the 8-agent `verification-report.md` (all in `~/Claude/Projects/FreeRTOS -- mender-mcu/`). Every code-level claim below was verified against `mender-mcu@af9b0b0` or upstream sources on 2026-07-06. MEN-9928 / MEN-9929 folded in (read via Jira API).*

## 0. TL;DR

**Epic framing (MEN-9928, "FreeRTOS i1: ESP32-S3 demo on ESP-IDF", split from journey epic MEN-8691):** iteration 1 is a *technical derisk demo* — Mender MCU builds and runs on the **latest supported ESP-IDF** on the ESP32-S3 reference board, documented in a **Mender Hub post** (+ maybe a GitHub README), **explicitly not officially supported**. Urgency: community FreeRTOS work is already landing at the repo's edges (PR #246) — the team wants planning *and* first tasks in the same sprint to avoid diverging forks.

mender-mcu is Zephyr-only today; nothing FreeRTOS exists anywhere in the org (verified — greenfield). FreeRTOS brands itself an RTOS, but what you compile in is a kernel plus à-la-carte libraries (coreHTTP, FreeRTOS-Plus-TCP, …) — no driver model, no board support, no integrated TLS/storage/bootloader, unlike Zephyr. So the port is mostly choosing and integrating companion components (HTTP, storage, bootloader), plus one genuinely new file — a FreeRTOS `os.c`, estimated **~350–450 lines**. The recommended shape (which the platform-directory layout already anticipates, and which mirrors Joel Guittet's upstream tree):

- `os/freertos/` written once against the vanilla-kernel API subset that also works on ESP-IDF's FreeRTOS fork
- `tls`/`sha` stay on the existing `generic/mbedtls` — reusable as-is (verified: no platform includes)
- `net`, `storage`, and the update module are per-platform: **ESP-IDF first** (everything needed ships in-box, Joel proved the path, same ESP32-S3 hardware as the Zephyr reference → direct footprint/behavior comparison), vendor-neutral (coreHTTP + MCUboot) later

## 1. Background (condensed, verified)

**FreeRTOS** = scheduler + primitives (tasks, queues, semaphores/mutexes, event groups, task notifications, software timers, pluggable heaps), compiled into the firmware, configured via `FreeRTOSConfig.h`. No network stack, no TLS, no filesystem, no HTTP, no bootloader. Latest kernel V11.3.0 (v11 added official SMP); LTS bundle 202604 ships kernel v11.3.0 and coreHTTP v3.1.3. In practice almost nobody runs bare FreeRTOS — they run a vendor SDK that *contains* it (ESP-IDF, MCUXpresso, STM32Cube, FSP, SimpleLink), each pre-integrating lwIP + mbedTLS + flash/OTA + bootloader. "The FreeRTOS ecosystem" is a family of similar-but-incompatible platforms sharing one kernel API.

**ESP-IDF** runs "ESP-IDF FreeRTOS" — a fork of vanilla **v10.5.1** with SMP modifications (core affinity via `xTaskCreatePinnedToCore`, spinlock critical sections via `portMUX_TYPE`, per-core tick/idle). Unchanged in IDF v6.0. `CONFIG_FREERTOS_SMP` (Amazon SMP kernel) still exists, still experimental. Practical consequence: code against the FreeRTOS API is mostly portable to IDF but must be tested there explicitly. **The v10.5.1-vs-v11.3.0 delta does not touch any API our os.c design needs** (queues, one-shot timers, mutexes, task create/delete are identical) — verified against the design in §4.

**IDF v6.0** is current stable (released 2026-03-20; 30-month support: 12 service + 18 maintenance). ESP32-S3 fully supported. Breaking changes relevant to us: `esp_ota_get_app_description`/`_elf_sha256` moved to `esp_app_format`, `esp_https_ota` partial download behind a config, crypto stack moved to **PSA Crypto** (dovetails with mender-mcu's fresh TF-PSA-Crypto support, ea85856), several legacy FreeRTOS compat APIs removed. Joel's client has an open PR (#132) for v6 support — still open with changes requested; v6 breaks v5-era integrations. **Recommendation (not decided — §7.2): IDF v6.0** — greenfield, longest runway, PSA alignment; keep Joel's v5-era code as reference only, not copy source. The honest counter-case: v6-first stacks three unproven things — the ecosystem is still migrating (Joel's v6 PR unmerged), v6's Mbed TLS 4.0/PSA-first layer has real costs (`psa_crypto_init()` required, ~800 B extra stack per HTTPS connection, <250-bit curves dropped), and mender-mcu's own TF-PSA-Crypto path (ea85856) landed 2026-07-01 unreleased — so on v6, "tls/sha reusable as-is" routes through our least-battle-tested code, while v5.5 (supported ~early 2028) routes through mature mbedTLS 3.x. The runway delta is only ~8 months. And the real question underneath: **v6-only, or a v5.5+v6 support matrix?** v6-only cuts off most of today's ESP32 install base; both = compat burden (cJSON sourcing, crypto path, removed APIs) that must be scoped up front. Phase 0 spike (step 6) exists to retire this cheaply.

**What ESP-IDF gives us per Mender need** (all verified):

| Need | ESP-IDF | Bare-FreeRTOS equivalent (later) |
|---|---|---|
| TCP/IP | lwIP (`esp_netif`) | lwIP or FreeRTOS-Plus-TCP |
| TLS | mbedTLS (`esp-tls`) | mbedTLS — mender's generic impl |
| HTTP client | `esp_http_client` (TLS, streaming reads, custom headers → JWT + Range OK) | coreHTTP — **caveat below** |
| KV storage | NVS (wear-aware; 15-char keys, our ≤1 kB of data fits trivially) | littlefs, FlashDB, raw flash |
| A/B + rollback | `esp_ota_*` + `otadata`, rollback behind `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` | MCUboot |
| Bootloader | IDF 2nd-stage (or in-tree MCUboot `boot/espressif` port — more mature than expected, Espressif published a getting-started Jan 2026) | MCUboot |
| CI without hardware | official QEMU for esp32/c3/s3 | Renode |

**coreHTTP caveat (load-bearing, verified):** no streaming response mode — the entire response must fit in the user buffer. Multi-MB downloads only work as an application-level loop of small Range requests (the AWS IoT OTA pattern). That happens to be exactly the shape of mender-mcu's existing ranged-download path, so a future vendor-neutral `http.c` composes naturally — but it means Range-loop logic is that backend's *only* mode.

## 2. What we're porting — the verified surface

Core (`src/core/`) is platform-independent, only third-party dep is cJSON (verified: no platform includes anywhere). Platform code under `src/platform/<surface>/<platform>/` with `generic/weak/` stubs. Headers in `src/include/`: `os.h`, `http.h`, `tls.h`, `sha.h`, `storage.h`, `update-module.h`, `certs.h`, `log.h`, `inventory.h`, `alloc.h`.

Per-surface effort for an ESP-IDF target:

| Surface | Status | Work |
|---|---|---|
| **os** | new file, fully specced (§4) | ~350–450 lines |
| **net/http** | reimplement `http.c` incl. `mender_http_artifact_download()` (non-ranged first; Range loop *is* platform code), **`mender_http_get_retry_interval()`** (429/Retry-After parsing — implemented only in zephyr http.c:711 today; without it the ported os-handler's rate-limit branch is a link error), and the `mender_http_recv_buf_length` global (core sizes the artifact parser buffer from it) | medium-large — the biggest genuinely new code surface; `esp_http_client` covers the transport |
| **tls, sha** | `generic/mbedtls` reusable as-is | ~0 (watch IDF v6 PSA interplay) |
| **storage** | ESP-IDF NVS; 5 items, ≤1 kB total, IDs map trivially | small |
| **deployment logs** | Zephyr uses FCB — **no ESP-IDF analog**; optional, already default-OFF in plain-CMake builds | skip first target |
| **update module** | new module via **public `mender_update_module_register()`** — there is no flash porting layer (flash.h removed in deb5c50) | medium; mapping table in §5 |
| **certs, log, inventory, alloc** | weak/generic fallbacks exist — but weak log is a silent no-op: write a small `esp_log`-backed `log.c` on day one (posix printf `log.c` is the 5-minute template). Watch PR #246 ("make logging definitions generic") — community prep for exactly this surface; coordinate, don't collide | low |
| **build** | `target/freertos/esp-idf/` (dir already created locally, empty); IDF component + Kconfig (IDF has Kconfig!) with `target/posix/CMakeLists.txt` as reference | small-medium |

## 3. Prior art — Joel's client (verified state)

`joelguittet/mender-mcu-client` — actively maintained (last push 2026-05-31). What to study vs what to copy:

- `platform/scheduler/freertos/src/mender-scheduler.c` (exactly 412 lines): per-work `xTimerCreate` periodic timers → `xQueueSend` → one worker task; per-work binary semaphore as overlap guard; mutexes on `xSemaphoreCreateMutex`; **blocking deactivate** (busy-wait timer stop, then take the semaphore); NULL-sentinel shutdown; no FromISR anywhere. Defaults: queue 10, stack 20480, prio 5. **Study the mechanics, don't copy the design** — our contract differs (§4).
- `platform/net/esp-idf/`, `platform/storage/esp-idf/nvs/`, `platform/flash/esp-idf/` — working `esp_http_client` / NVS / `esp_ota_*` integrations. His flash layer uses exactly the `esp_ota_*` calls our update module needs — direct reference for the mechanics.
- **Gaps vs our fork** (both verified): no Range/resume support at all, and no work-handler retry/backoff logic — both are fork additions that live in *platform* code and must not be lost.
- His fork also has `tls/generic/psa_crypto`, `storage/generic/psa_its` + `psa_ps`, cryptoauthlib (ATECC608) — dropped in our fork, likely future customer asks; PSA is the portable route.
- Examples: `mender-esp32-example` (ESP-IDF end-to-end), `mender-stm32h745-zephyr-example` (out-of-tree port via weak functions + ATECC608), `mender-ncs-example` (nRF Connect, PSA).

## 4. The FreeRTOS os.c — settled design (deep-verified)

Contract facts from core (all file:line-verified; details in verification report §2):

- Only **two work items ever exist** (client main, inventory). Queue depth 2–4 suffices.
- Everything heavy runs in work callbacks on the scheduler stack: TLS keygen, auth, **full artifact download**, update-module callbacks. Zephyr stack default 6 kB; budget **8–16 kB** on FreeRTOS.
- `work_execute` must be a **non-blocking enqueue callable from inside a work callback** (client work triggers inventory work).
- `work_set_period` has no core callers — stub.
- **No ISR-context callers** — no FromISR variants.
- Mutexes: two in core, infinite-timeout, never recursive — `xSemaphoreCreateMutex` suffices.
- `mender_os_sleep` is called inside work context (retry backoff) → `vTaskDelay`.
- **Zephyr and POSIX os.c have diverged**: exponential backoff on `MENDER_RETRY_ERROR`, 429 rate-limit retry via `mender_http_get_retry_interval()`, and `MENDER_DONE`-stops-rescheduling live in the *Zephyr work handler* only (zephyr/os.c:219-272). **Port that handler logic or retry behavior silently disappears.** (Don't copy its one wart: backoff reset value in a single global — keep per-work.)
- Deactivate semantics: Zephyr's is a *non-blocking cancel* (drain only at `scheduler_exit`); POSIX's blocks until in-flight work finishes. **Implement the blocking POSIX semantics** — exit paths free the work right after deactivate (this is a latent UAF on Zephyr today; separate ticket).

Design: dedicated worker task + queue + **one-shot timers re-armed by the worker after the function returns** (Zephyr's end-to-start model), NULL-sentinel shutdown. The deactivate/re-arm interlock is the hard part of the file — review-verified requirements:

- **Per-work pending flag/semaphore taken at *enqueue* time** (in the timer callback and in `work_execute`), rolled back if `xQueueSend` fails — exactly what POSIX does (posix/os.c:308-326). Taking it only when execution *starts* leaves a queued-but-not-started item invisible to deactivate → deactivate returns, exit frees the work, worker dequeues a dangling pointer.
- Enqueue-time dedup bounds queue occupancy to ≤1 per work, so **depth ≥ #works makes send failure impossible** — necessary, because under one-shot re-arm a dropped enqueue means the timer is never re-armed and the periodic work goes silent forever (no auto-reload safety net; "depth 2–4 suffices" is only true *with* the dedup).
- **Worker checks `activated` (cleared by deactivate) before re-arming**, and deactivate accounts for `xTimerStop` being *asynchronous* (daemon-queued — a pending expiry can still run after it returns, unlike POSIX `timer_settime`): stop the timer again after acquiring the pending semaphore; `xTimerDelete` in `work_delete`.
- **Activate fires the work immediately** (both existing impls do: `K_NO_WAIT` / direct callback invocation) — arming at full period delays the first update check by up to the poll interval — and must restart a work left dormant by `MENDER_DONE`.
- **Deactivate self-deadlock guard**: the app restart callback runs in worker context and may call `mender_client_exit()` — `if (xTaskGetCurrentTaskHandle() == worker) skip the wait` (POSIX has this latent bug today).

`xTimerPendFunctionCall` was evaluated and **ruled out**: work blocks for minutes → stalls every software timer in the system; daemon stack would need to carry mbedTLS+cJSON; blocking deactivate unimplementable against the daemon task.

## 5. Update module — state mapping (deep-verified)

**10 callback states** (+ `END` sentinel): `DOWNLOAD, INSTALL, REBOOT, VERIFY_REBOOT, COMMIT, CLEANUP, ROLLBACK, ROLLBACK_REBOOT, ROLLBACK_VERIFY_REBOOT, FAILURE`. Normal path DOWNLOAD→…→COMMIT→CLEANUP; state persisted before reboot-ish callbacks; resume lands in VERIFY_REBOOT; loop-breaker at 28 state stores.

| State | Zephyr/MCUboot | ESP-IDF |
|---|---|---|
| DOWNLOAD | `flash_img_init` / `flash_img_buffered_write` | `esp_ota_begin` / `esp_ota_write` / `esp_ota_end` |
| INSTALL | `boot_request_upgrade(BOOT_UPGRADE_TEST)` | `esp_ota_set_boot_partition` + **`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` as hard requirement** (gives PENDING_VERIFY = test semantics; without it the state is UNDEFINED and VERIFY_REBOOT breaks) |
| REBOOT / ROLLBACK_REBOOT | app restart callback | `esp_restart()` |
| VERIFY_REBOOT | `boot_is_img_confirmed()` — FAIL if already confirmed | success iff running partition state == `ESP_OTA_IMG_PENDING_VERIFY` |
| COMMIT | `boot_write_img_confirmed()` (fails `MENDER_NOT_FOUND` if no image pending) | `esp_ota_mark_app_valid_cancel_rollback()` **guarded by** running-state == `ESP_OTA_IMG_PENDING_VERIFY` — the ESP call returns OK on an already-valid image, which would silently swallow the Zephyr module's no-pending-image error path |
| ROLLBACK | no-op check (MCUboot auto-reverts unconfirmed image) | **needs explicit action**: `esp_ota_mark_app_invalid_rollback_and_reboot()` (collapses ROLLBACK+ROLLBACK_REBOOT) or `esp_ota_set_boot_partition(previous)`. **Pre-reboot failure (post-INSTALL) also needs action**: `esp_ota_set_boot_partition(running)` to undo INSTALL's otadata switch — doing nothing means the next power cycle boots the unverified image (Zephyr's equivalent is FAILURE's slot1 erase cancelling the pending swap) |
| FAILURE | free handle; erase slot1 if confirmed | `esp_ota_abort()` if handle open |
| CLEANUP, ROLLBACK_VERIFY_REBOOT | no callback registered | **CLEANUP needs a callback on ESP-IDF**: DOWNLOAD failures transition to CLEANUP, *not* FAILURE (client.c:113) — the most common failure class (network abort, SHA mismatch) would otherwise leave the `esp_ota` handle open, keeping the partition in-use and potentially failing the next `esp_ota_begin` |

Port the Zephyr module's guards too, not just the happy path: NULL-filename skip (payload-less artifacts deliver no file), the `artifact_had_payload` check that fails INSTALL when nothing was flashed (else `esp_ota_end`/`set_boot_partition` on an empty handle), and stale-handle handling at DOWNLOAD offset==0.

Data path: artifact streams in ≤512 B blocks to the DOWNLOAD callback, SHA-256 in the same pass, **no decompression** (compressed artifacts actively rejected — build with `--compression none`). `esp_ota_write` handles unaligned writes, no extra buffering. Hosted-Mender note: MCU devices land in the **micro device tier** — 5 MB artifact cap (the *full uncompressed* IDF app image counts against it; typical Wi-Fi+TLS apps at 1–2 MB fit) and long default polling intervals (1 d update / 14 d inventory — override for demos/testing).

Ranged download (`CONFIG_MENDER_ARTIFACT_DOWNLOAD_RANGES`, default **n**): the whole loop is platform code — Range GETs, require 206, total from `Content-Range`, per-chunk reconnect+retry, CONNECTED once / DATA_RECEIVED per fragment / DISCONNECTED once (parser must NOT reset between chunks). Resume intra-session only. On ESP-IDF: set the Range header via `esp_http_client_set_header`; `esp_http_client_get_content_length` returns chunk length, total needs `Content-Range` header inspection.

Storage: 5 NVS items (keypair, deployment data, provides, artifact name), ≤1 kB total, ~8 writes per successful deployment. ESP-IDF NVS dedups identical writes (matches `checked_nvs_write` for free). Stock 24 kB `nvs` partition just works.

## 6. The plan

### Phase 0 — learn + environment (the "finding out" phase)

1. **FreeRTOS book** (freertos.org/Documentation/RTOS_book.html): tasks, queues, software timers, task notifications chapters — enough to write and review os.c. Skim heap models (`heap_4`/`heap_5`; IDF uses its own `heap_caps`).
2. **ESP-IDF getting started, hands-on, IDF v6.0** — go through the official guide, not just an install: toolchain + `idf.py` on the ESP32-S3-DevKitC you have, build/flash/monitor `hello_world`. This validates the environment and teaches the idf.py/Kconfig/partition-table workflow the port will live in.
3. **OTA example next** (`examples/system/ota/simple_ota_example`, then the rollback-enabled variant): two-slot partition table, `esp_ota_*` flow, `PENDING_VERIFY` → confirm/rollback behavior on real hardware. This is the update module's backend — worth a day of poking (deliberately mark an image invalid, watch it roll back).
4. **Read the IDF FreeRTOS fork deltas** (freertos_idf docs page) — know which vanilla idioms don't transfer.
5. **Study Joel's tree side-by-side with ours**: his `scheduler/freertos` vs our `src/platform/os/{zephyr,posix}/os.c`; his `net/storage/flash` esp-idf layers vs our `http.h`/`storage.h`/update-module contracts. One sitting, notes in the project folder. (License note: Joel's client is MIT — if anything gets copied rather than re-derived, attribution rides along. ESP-IDF's Apache-2.0 + Wi-Fi/PHY blobs don't matter for a client library — we don't redistribute IDF, only reference *images* embed blobs.)
6. **Spike: compile `generic/mbedtls` tls/sha inside the IDF v6 hello_world project and run keygen/sign/SHA.** Retires the plan's central reuse assumption ("tls/sha as-is") against v6's Mbed TLS 4.0/PSA layer *before* any porting starts, and feeds §7.2 with facts instead of vibes.

### Phase 1 — internalize the client (parallel with Phase 0, not a gate)

~~POSIX~~ — reviewer-verified: `target/posix/` is **library-only** (no `main()` anywhere, no posix update module), so "run a deployment on POSIX" means writing an unbudgeted harness first. Instead: use **mender-mcu-integration's `native_sim` board** — gdb-able on host, noop-update module already wired. Run a full deployment against a Mender server (hosted or demo) under a debugger; step the state machine end to end incl. a forced failure → rollback path. Learning task + regression baseline — core must not change during the port.

### Phase 2 — os/freertos

Write `src/platform/os/freertos/os.c` per §4. Two sub-decisions already made: dedicated worker task (config macros mirroring `CONFIG_MENDER_SCHEDULER_WORK_QUEUE_{STACK_SIZE,PRIORITY}`), blocking deactivate with the §4 interlock. This phase includes a **minimal IDF component + test app** — just enough to run os.c on target/QEMU (full Kconfig/platform selection stays in Phase 3; without this scaffold there is nothing to debug on). Settle dependency sourcing here: cJSON is a **managed component** in IDF v6 (`espressif__cjson` — literally half of Joel's v6 PR), mbedTLS comes from IDF's bundled copy, never mender-fetched. Note: the ported handler's 429 branch calls `mender_http_get_retry_interval()` — only the weak http backend exists until Phase 3, so that branch is dead code until then (expected; link against the weak backend). If os.c bugs prove hard to chase on target/QEMU, the kernel's posix port makes a cheap host harness — don't build one preemptively.

### Phase 3 — build target + net + storage on ESP-IDF

- `target/freertos/esp-idf/`: full IDF component — platform selection + Kconfig (grow the Phase 2 scaffold; the Zephyr module and **`cmake/CMake_defaults.txt`** are the references — not `target/posix/CMakeLists.txt`). Critical: replicate the CMake defaults — `CONFIG_MENDER_FULL_PARSE_ARTIFACT`, `PROVIDES_DEPENDS`, `COMMIT_REQUIRE_AUTH` all ON. Core compiles fine with zero `CONFIG_MENDER_*` defined, but full-parse gates the artifact/device-type compatibility check — miss it and the port *silently skips artifact compat verification*.
- Add the IDF build job to CI the moment the component builds (end of this phase).
- `src/platform/net/esp-idf/http.c`: `esp_http_client`. First cut of `mender_http_artifact_download()` = the non-ranged path — single GET streamed via `esp_http_client_read` (ranges are default-off on Zephyr too); add the Range loop (§5) only when/if §7 decision 5 lands on default-on. Keep TLS via IDF's bundled mbedTLS so `generic/mbedtls` tls/sha drop in unchanged; watch v6's PSA layer.
- `src/platform/storage/esp-idf/nvs/storage.c`: 5 items; deployment logs compiled out.
- Milestone: device authenticates, sends inventory, polls deployments — with the **noop-update pattern** from `mender-mcu-integration`'s `native_sim` board (no OTA backend yet). Full client loop, no flashing.

### Phase 4 — esp-idf update module

New module registered via `mender_update_module_register()`, `artifact_type = "esp-idf-image"` — working name; **the string freezes the moment a customer creates artifacts with it**, so it must be confirmed before first release (server/mender-artifact accept arbitrary types — matching is on `device_type`, so there's no server-side constraint on the choice). Rollback-enable as a hard build requirement. Milestone: full OTA deployment ESP32-S3 → new image → test boot → commit; and the failure path: deploy a self-failing image, watch `PENDING_VERIFY` revert + Mender marks deployment failed. Record footprint vs the Zephyr reference app on the same board — a data point for docs, not a gate (it mostly measures the two RTOS stacks).

### Phase 5 — reference app + CI

- ESP-IDF equivalent of `mender-mcu-integration` (separate repo or `target/freertos/esp-idf/example/` — decide with the team; MEN-9928's "maybe README in a github repo" hints separate repo, Joel's `mender-esp32-example` is the shape).
- CI: posix smoke stays; IDF build job already added end of Phase 3. QEMU (esp32s3) boot smoke maybe; full deployment-loop-in-CI (QEMU + mock server) is its own project — add when the first on-target regression escapes, not before.
- Docs: the **Mender Hub post** (epic acceptance criterion): how to build/run, partition-table requirements, artifact creation (`--compression none`), explicitly-unsupported disclaimer. Plus README note in mender-mcu (and fix the stale Zephyr 4.2.0-only row while there).

### Later / explicitly deferred

- **Vendor-neutral FreeRTOS target** (coreHTTP + MCUboot + littlefs/FlashDB): os.c and the update-module *pattern* carry over; coreHTTP backend = Range-loop-only by design (§1 caveat); MCUboot's in-tree espressif port allows proving vendor-neutral on the same ESP32-S3 hardware. Don't lean on Lab-Project-FreeRTOS-MCUBoot — dead since 2021, now marked reference-only.
- Deployment logs on ESP-IDF (hand-rolled ring buffer on `esp_partition`, or upstream ask).
- PSA / secure-element backends (Joel's tree is the reference).
- Separate ticket, not this epic: the Zephyr exit-path UAF (non-blocking deactivate → `work_delete` frees while handler may run).

## 7. Open decisions for the team

1. ~~First target framing~~ — **resolved by MEN-9928**: ESP32-S3 demo on ESP-IDF is the epic. Vendor-neutral belongs to later MEN-8691 iterations.
2. IDF version — **largely resolved by MEN-9928**: acceptance says "latest supported ESP-IDF" = v6.0 today. §1's counter-case stands as risk documentation (Phase 0 spike still worth running); the v5.5+v6 support-matrix question defers to later iterations — i1 is unofficial, latest-only.
3. Where the reference app lives (mender-mcu-integration sibling vs in-repo example).
4. Ranged downloads for the ESP-IDF target: default-off like Zephyr (then the simple single-GET download suffices and the Range loop isn't built yet) vs default-on (chunked Range is the resilience story on flaky Wi-Fi, but costs the full loop implementation up front).
5. `artifact_type` string — must close before first release (frozen once customer artifacts exist; Phase 4 works under `"esp-idf-image"`).

## 7b. Done, sizing, release, maintenance

- **Epic done (per MEN-9928 acceptance):** Mender MCU builds and runs on latest supported ESP-IDF (Phase 4 milestone on hardware — deploy + commit + revert — is the honest bar for "runs") + how-to documented in a **Mender Hub post**, maybe a GitHub README. **Not officially supported** — no docs.mender.io section, no support commitments. A "demo, not a port" is literally the epic's intent; the product-grade port is later MEN-8691 iterations.
- **Relative sizing:** Phase 0+1 small (a week-ish of learning, parallelizable); Phase 2 medium (~400-line file, but §4's interlock is subtle); **Phase 3 is the biggest** (http.c is the largest new surface + component/Kconfig plumbing) ≈ 2× Phase 2; Phase 4 medium (mapping is specced, guards are the work); Phase 5 small-medium (Hub post instead of official docs).
- **Release vehicle:** merge to `main` as it goes (Vratislav's explicit rationale — in-repo to avoid diverging forks and PR-#246-style friction); additive platform, no public-header changes → rides normal v1.x releases as unsupported/experimental. No dedicated release gate for i1.
- **Maintenance:** IDF build job on every PR (cheap, protects the target from Zephyr-side breakage); on-target/QEMU stays manual for i1. Toolchain-image ownership + Espressif release-cadence tracking become real when a later iteration makes this officially supported.

## 8. Reading list (ordered)

1. FreeRTOS book — tasks, queues, software timers, notifications
2. ESP-IDF get-started (v6.0, esp32s3) + `simple_ota_example` + partition tables + app image format
3. ESP-IDF build-system/component + component-manager docs (the port *ships as* an IDF component — Phase 3's core deliverable), `esp_http_client` API docs (biggest new code surface), and the v5→v6 migration guide (authoritative breaking-changes catalog)
4. ESP-IDF FreeRTOS overview + IDF-FreeRTOS (SMP) fork details
4. Joel: `platform/scheduler/freertos/`, `platform/{net,storage,flash}/esp-idf/`, `mender-esp32-example`
5. Ours: `src/platform/os/{zephyr,posix}/os.c` (with verification report §2 open beside it), `src/platform/update_modules/zephyr/image/update-module.c`, `src/platform/net/zephyr/http.c:450-641` (the Range loop), `docs/flashwear.md`
6. MCUboot design doc + `boot/espressif` readme (for the deferred vendor-neutral phase)
7. FreeRTOS-LTS 202604 / coreHTTP (same)
