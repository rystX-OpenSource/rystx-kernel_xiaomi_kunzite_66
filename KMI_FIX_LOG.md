# KMI_FIX_LOG — ZRAM/ZSMALLOC/ZPOOL backport (GKI 6.6, android15-6.6-lts lineage)

Branch: `rystx-re-staging-sched` (kunzite-66, kernel 6.6.142)
ABI baseline: **79b34f73a7fa** (`certs: Guard PKCS#11 PIN handling…`), the parent of
the first backport commit `4463c1b27fbe` (`zsmalloc: checkout to android15-6.6-desktop`).
Verification: **object-level only** (no full build, link, modpost or boot — see "Coverage & gaps").

## Baseline validation

The scratch config (raccoon_defconfig, `MODVERSIONS=y`, `TRIM_UNUSED_KSYMS=n`, `CFI_CLANG=n`,
`LTO_NONE`) is correct: baseline CRCs are deterministic run-to-run and match the committed
`android/abi_gki_aarch64.stg` exactly for every symbol checked
(e.g. `zs_map_object=0xc341ae6d`, `zs_unmap_object=0x924c46f8`, `__cpuhp_setup_state=0x97fa317c`).

## Findings

| # | Finding | Kind | Introducing commit (mainline) | Reachable by vendor modules? | Fix |
|---|---------|------|-------------------------------|------------------------------|-----|
| 1 | `zs_map_object`, `zs_unmap_object`, `enum zs_mapmode` removed from `zsmalloc.h`/`zsmalloc.c`; replaced by `zs_obj_read_begin/read_end/write` | removed exported symbols + removed enum | `242f1af248a5` *mm: zsmalloc: remove object mapping APIs and per-CPU map areas* (mainline ~7.3) | **Yes** — `zs_map_object`/`zs_unmap_object` on `abi_gki_aarch64_pixel`, `_amlogic`, `_exynos` and in `.stg` | Compat shim: restore enum + both exports over new per-zspage locking, keep per-CPU copy buffer + `CPUHP_MM_ZS_PREPARE`. New `zs_obj_*` kept additive. (commit `f2e28eeeff0f`) |
| 2 | `CPUHP_MM_ZS_PREPARE` deleted from `enum cpuhp_state` → all later enumerators renumber → CRC change on `__cpuhp_setup_state`, `__cpuhp_setup_state_cpuslocked`, `__cpuhp_state_add_instance`, `__cpuhp_remove_state`, `__cpuhp_state_remove_instance` | changed enum → CRC-breaking for exported cpuhp funcs | `242f1af248a5` (same commit) | **Yes** — all five on `_pixel`/`_amlogic`/`_exynos` | Re-add enumerator in original slot (value 64). Reused by the restored zsmalloc shim, so not dangling. (commit `5d6ec657d128`) |
| 3 | `zpool_map_handle`, `zpool_unmap_handle`, `zpool_malloc_support_movable`, `zpool_can_sleep_mapped`, `enum zpool_mapmode` removed; `struct zpool_driver` layout changed | removed exports + changed struct | `f27b8864fed5` *mm: zpool: remove object mapping APIs* | **No** — none of these on any symbol list or in `.stg` | No ABI fix required. Documented. |
| 4 | `zpool_register_driver`/`zpool_unregister_driver` CRCs changed (`0xb2433bb9→0x1604a030`, `0xda6768ec→0x45c98315`) via the `struct zpool_driver` layout change | CRC-only | `f27b8864fed5` | **No** — not on any list or `.stg` | No ABI fix required. Documented. |
| 5 | `mm/zswap.c` still calls removed `zpool_map_handle`/`zpool_unmap_handle`/`zpool_can_sleep_mapped` | latent compile break | `f27b8864fed5` | n/a (not KMI) | **Latent, out of scope.** `CONFIG_ZSWAP` is off in raccoon_defconfig and gki_defconfig, so `zswap.o` is not built. Would need the upstream obj-API conversion before enabling ZSWAP. Not fixed. |

## Resolution detail

### Finding 1 — zsmalloc map-API shim (`f2e28eeeff0f`)
- Why not "eliminate cause" (revert the hunk): zram's own code was converted to the new
  `zs_obj_*` helpers by the backport and the new per-zspage sleepable locking is wanted; reverting
  would drop backported functionality. So option 2 (compat shim) is correct.
- The shim calls `read_lock(&pool->lock)` (the lock became an `rwlock_t` in the backport) then
  `zspage_read_lock()`, mirroring the new `zs_obj_read_begin()` path, and keeps the per-CPU
  `vm_buf` copy buffer + `__zs_map_object`/`__zs_unmap_object` for cross-page objects, exactly as
  the baseline did. Signatures and types are byte-identical to the baseline, so the CRC is
  identical.
- `zs_obj_read_begin/read_end/write` are left exported (additive). They are on **no** symbol list,
  so they do not affect the KMI; zram imports them.

### Finding 2 — cpuhp enumerator restore (`5d6ec657d128`)
- `enum cpuhp_state` is unnumbered; deleting a middle value shifts every subsequent value, which
  changes the type-hash CRC of every exported function whose type graph includes `enum cpuhp_state`.
  Re-adding `CPUHP_MM_ZS_PREPARE` in its original position restores all five CRCs.
- Not dangling: the restored zsmalloc shim's `zs_init()` calls
  `cpuhp_setup_state(CPUHP_MM_ZS_PREPARE, …)`.

## Object-level CRC results (after fixes)

zsmalloc exports — HEAD vs baseline (identical for every common symbol; HEAD adds only the three additive helpers):

```
                        baseline      HEAD
zs_compact              0x5da67adc    0x5da67adc
zs_create_pool          0xbcf1f0e6    0xbcf1f0e6
zs_destroy_pool         0xc69b7ee5    0xc69b7ee5
zs_free                 0x958df3ac    0x958df3ac
zs_get_total_pages      0xcbe56bc2    0xcbe56bc2
zs_huge_class_size      0x3d8baf3b    0x3d8baf3b
zs_lookup_class_index   0x5957cddb    0x5957cddb
zs_malloc               0xb29533ee    0xb29533ee
zs_map_object           0xc341ae6d    0xc341ae6d   (restored)
zs_unmap_object         0x924c46f8    0x924c46f8   (restored)
zs_pool_stats           0x7cceaf92    0x7cceaf92
zs_obj_read_begin         —           0x73243049  (additive, no list)
zs_obj_read_end           —           0x3aed7395  (additive, no list)
zs_obj_write              —           0x4d119946  (additive, no list)
```

cpuhp exports — kernel/cpu.o, HEAD vs baseline (identical after fix):

```
__cpuhp_remove_state            0x19ac0ff7   (was 0x8bb177f2 before fix)
__cpuhp_setup_state             0x97fa317c   (was 0x236fa44c before fix)
__cpuhp_setup_state_cpuslocked  0xb2fdc891   (was 0x28dbb6a1 before fix)
__cpuhp_state_add_instance      0xaa87cf19   (was 0x914fe9d1 before fix)
__cpuhp_state_remove_instance   0x8fa8aca1   (was 0xa7a47b4a before fix)
```

zpool exports (not KMI — informational): `zpool_has_pool` unchanged `0x6005c351`;
`zpool_register_driver` `0x1604a030`, `zpool_unregister_driver` `0x45c98315` changed but on no list.

## Imported-symbol check

New undefined symbols introduced in `mm/zsmalloc.o` vs baseline: `copy_page`, `flush_dcache_page`,
`kasprintf` — all three exist in the baseline and are in `abi_gki_aarch64.stg`. zram_drv.o imports
`zs_malloc/zs_free/zs_compact/zs_create_pool/zs_destroy_pool/zs_get_total_pages/zs_huge_class_size/
zs_lookup_class_index/zs_pool_stats` (unchanged CRCs) and the additive `zs_obj_*`. No dropped
imports. CRC of each imported symbol verified where the defining object was built
(zsmalloc, zpool, cpu); the general vmlinux-wide imported set is **not verified (needs full build)**.

## Android hooks / KABI fields preserved (Rule 3)

- `mm/zsmalloc.c`: `trace_android_vh_zs_shrinker_bypass`, `trace_android_vh_zs_shrinker_adjust` —
  both present at the same call sites as the baseline. No `ANDROID_KABI_*`/`ANDROID_VENDOR_DATA`
  in these files in either baseline or HEAD.
- No vendor hooks or reserved fields were removed by the fixes.

## Additive symbols introduced

`zs_obj_read_begin`, `zs_obj_read_end`, `zs_obj_write` — new exports from the backport, kept.
They are on **no** symbol list, so they do not alter the KMI; they are internal-to-tree (zram).
No new entry was added to any `abi_gki_aarch64*` list or to the STG.

## Verification coverage & gaps

Verified (object-level, scratch `O=` build, genksyms via MODVERSIONS, CFI/LTO off):
- Exported-symbol CRC equality for zsmalloc and cpuhp (the two KMI-reachable findings).
- Source-level exported-surface diff; symbol-list reachability of every changed symbol.
- Imported-symbol diff for zsmalloc; defining-object CRCs for the bounded core set (cpu.o).
- All touched objects compile with no new warnings/errors under the branch flags.

**Not verified (by design — needs full build):**
- Link and modpost: run `make … modules` / `vmlinux` to catch `Unknown symbol` and modpost errors.
- `TRIM_UNUSED_KSYMS` interaction (scratch build disables it): build with the real GKI config.
- Full-tree ABI diff: needs a complete `vmlinux` with debug info + STG/abidw.
- Module load, boot, runtime behaviour: on-device test.
- Suggested later commands (not run here):
  - `make LLVM=1 ARCH=arm64 O=out raccoon_defconfig && make LLVM=1 ARCH=arm64 O=out -j$(nproc)`
  - `make LLVM=1 ARCH=arm64 O=out modules_prepare modules`
  - GKI ABI: the branch's Kleaf/Bazel `…_abi` target or `scripts/gen_ksymtab`/STG `stgdiff`
    against `android/abi_gki_aarch64.stg`.

## Tooling (in `tools/kmi/`, not committed)
- `kmake.sh` — single-object scratch build (clang-llvm/, ld.lld with NDK libxml2.so.2, bc shim).
- `bc-shim.py` — minimal `bc` for `kernel/time/timeconst.bc` (host has no `bc`).
- `crc.sh` — replays `CPP -D__GENKSYMS__ <c_flags> | genksyms` to print `sym crc` per object.
