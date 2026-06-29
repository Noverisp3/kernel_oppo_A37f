# Cinnamon Kernel Changelog

## Build #376

### eMMC HS400 + SD Card DDR50 (2026-06-29)

- **eMMC (sdhc_1):** Added `HS400_1p8v` bus-speed-mode + 200 MHz clock rate. HS400 doubles eMMC throughput vs HS200 at same clock.
- **SD card (sdhc_2):** Added `DDR_1p8v` bus-speed-mode enabling UHS DDR50 (50 MB/s vs base 25 MB/s SDR).
- Hardware will fall back to supported mode if the new modes fail.

## Build #372–#374 — Reverted

### PLL config_ctl_val Experiment — Failed (2026-06-29)

**Attempt:** CPU 1363 MHz via PLL CONFIG_CTL register (offset 0x14) with `config_ctl_val = 0x000D6968` (Snapdragon 810 SR2 PLL reference).

**Problem found:** `__variable_rate_pll_init` never called for SR2 PLL type — config_ctl_val not written to hardware. Fixed by adding init to `local_pll_clk_set_rate`.

**Result:**
- Config_ctl_val now takes effect (confirmed by performance change)
- 1363 MHz: **1106 ev/s** (half of 1209 MHz baseline) — wrong VCO settings corrupt PLL
- 1209 MHz: -6% regression (2144 ev/s) from init's USER_CTL side effects
- Writing 0 to CONFIG_CTL → **bootloop** (PLL needs valid analog config)

**Conclusion:** SR2 PLL VCO on 28nm LP is physically capped at ~1.2 GHz. No software PLL tuning can extend this.

**Revert:** All changes reverted in commit `e453afbfc4d`.

### Interactive Governor Tuning + I2C 1MHz (2026-06-29)

- **Interactive governor tuned** for faster ramp-up and UI smoothness:
  - `go_hispeed_load`: 90 → **75** (boost sooner)
  - `hispeed_freq`: 998400 → **1209600** (boost straight to max)
  - `min_sample_time`: 50ms → **30ms** (faster freq step-down)
  - `timer_rate`: 30ms → **20ms** (more frequent eval)
  - `above_hispeed_delay`: 25ms → **20ms**
  - `boostpulse_duration`: 60ms → **80ms** (longer touch boost)
  - `target_loads`: more aggressive curve (200MHz:37→25, ... 1209MHz:90→80)
  - Persisted via 120s reapply work (survives Android power HAL override)
- **I2C touch bus (QUP5)**: 400kHz → **1MHz** (`msm8916-common-15399.dtsi:105`). Driver already supports 1MHz divider table — only DTS change needed
- Sysbench CPU: 2286 → **2373 ev/s** (+3.8%)

## Build #368–#357 (Reverted)

### schedutil Governor Experiment — Reverted (2026-06-29)

**schedutil governor backported** from mainline concept. Uses scheduler UTIL_EST demand → frequency: `freq = max_freq * util / 1024`.

**Problems discovered:**
- **sleeping-in-softirq**: `__cpufreq_driver_target()` may sleep (mutex) → called directly from timer softirq → BUG splat. Fixed with workqueue deferral.
- **cpufreq sysfs hang**: `del_timer_sync()` on BSS-zeroed uninitialized timer (`timer->base = NULL`) → `spin_lock_irqsave(0)` infinite spin. Fixed by initializing timer before GOV_START.
- **UTIL_EST = 0 for CPU-bound tasks**: Tasks that never sleep have zero UTIL_EST → governor selects `policy->min` (200 MHz). Added `nr_running` fallback → per-CPU `nr_running=1` maps to only 25% max → still too slow.
- **schedule_work_on() starvation**: Work queued on busy CPU never runs → frequency never updates. Fixed with unbound `schedule_work()`.
- **Still 3× slower than interactive** (805 vs 2362 ev/s sysbench) even after all fixes.

**Root cause**: UTIL_EST only updates on task sleep/wake. CPU-bound workloads never sleep → zero signal. The HMP scheduler has no PELT-like continuous load tracking → any frequency scaling based on UTIL_EST is fundamentally broken for compute-bound tasks on this kernel.

**Conclusion**: schedutil cannot replace interactive on this kernel without backporting full PELT (thousands of lines). **Reverted to interactive governor** as default. schedutil remains available as an opt-in for testing.

### What's left in the code:
- `CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y` stays enabled
- schedutil governor module remains compiled
- `sched_cpu_util()` function stays exported for future use
- **Default governor remains `interactive`**
- Switch manually: `echo schedutil > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`

## Build #356

- **UTIL_EST**: Exponentially Weighted Moving Average (EWMA) of task utilization, backported from Linux 7.2-rc1. Prevents frequency collapse during brief task sleeps (~100 lines, `kernel/sched/fair.c`)
- **sched_min_granularity**: 750µs → **500µs** (runtime 1.5ms, 33% shorter time slices)
- **sched_wakeup_granularity**: 1ms → **250µs** (runtime 750µs, 4x faster wakeup preemption)
- **GENTLE_FAIR_SLEEPERS**: thresh >>=1 → **>>=2** (50% less sleeper credit, fairer scheduling)
- **CPU undervolt**: -25mV → **-50mV** on corners 3-8 (533-1209 MHz)

## Build #355

## Build #354

- **Removed fake CPU frequencies 1363 MHz and 1497 MHz** from `apcs_pll_freq[]`, DTS speed-bin tables, cpufreq-table, cpu-to-dev-map, and CPR corner map
- **Confirmed**: VCO=1 writes correctly (bit 28=1 in USER register) but PLL physically caps at ~1.2 GHz — CPU performance identical at 998, 1209, 1497 MHz (sysbench: ~1480 ev/s)
- **Max CPU frequency**: 1209600 kHz (corner 8)
- **GPU OC 620 MHz** preserved: uses RCG mux+divider from GPLL2, different mechanism from CPU PLL, likely real
- Updated hispeed_freq from 1497600 → 1209600

## Build #351

### Adreno Idler + Wakelock Blocker (2026-06-28)

- **Adreno Idler**: GPU stays in NAP (clocks off, rail on) for N idle cycles before deep sleep. Reduces SLEEP→ACTIVE thrashing during light GPU use. Sysfs: `gpu_idler` (on/off), `gpu_idler_idleworkload` (cycles, default 10)
- **Wakelock Blocker**: `/proc/wakelock_blocker` — block known battery-draining wakelocks (`+name` to add, `-name` to remove). 15 wakelocks blocked by default: wlan_wake, wlan_rx_wake, wlan_ctrl_wake, wlan_ipa, wlan_pno_wake, netmgr_wake, IPA_WS, qcom_rx_wakelock, ss_route_work, radio-interface, qcril, ims_socket_wake, CNE_WAKELOCK, cne_wqe_wake, power_policy_wake
- GPU sysfs permissions raised to 0666 for userspace write access
- Auto-enabled at boot via Cinnamon_Active Step 6–7

## Build #350

### Dynamic Fsync (2026-06-28)

- **vfs_fsync_range hook**: `/proc/dynamic_fsync` (1=skip fsync, 0=normal). Battery > 30% and not charging → skip fsync (faster I/O). Battery ≤ 30% or charging → normal fsync (data safe)
- Cinnamon_Active monitors battery every 60s, auto-toggles based on capacity
- Safe design: re-enables fsync on low battery, charging, or suspend

## Build #349

### TCP Westwood+ (2026-06-28)

- **TCP Westwood+** enabled as default congestion control (replaces CUBIC). Bandwidth estimation, better suited for WiFi/mobile (no slowdown from signal noise)
- Available: westwood, reno, bic, cubic, htcp
- Localversion: `-lineageos` → `-cinnamon`
- `lineageos_a37f_defconfig` removed, only `cinnamon_A37f_defconfig`

## Build #346

### Thermal Relax

- CPU frequency throttle: 60°C → **70°C**
- Core limit throttle: 80°C → **85°C**
- Hotplug/freq-mitigation: 94°C → **100°C**

## Builds #344–#345

### Panel Overclock 65fps + Touch Boost

- **Panel 60→65fps**: pclk=89,202,880 Hz, DSI VCO=356,811,520 Hz, bit clock=714 MHz (+6.6%). Modified BOE ILI9881C, TM NT35521S, Truly NT35521S panels. Reduced VBP/VFP
- **Fix**: removed `cont-splash-enabled` from panel DTS (blocking DTS timing)
- **Touch I2C 400kHz**: QUP5 bus 100kHz→400kHz
- **Synaptics**: `nosleep=1`, `report_rate=1` in wake function
- LM3630 PWM backlight: I2C dimming mode, `pwm-active=0`, PWM frequency hardware-fixed

## Builds #341–#343

### BIMC Clock OC Attempt (Abandoned)

- `of_clk_get_from_provider()` with `qcom,rpmcc-8916` DT node succeeded (build #341)
- `clk_round_rate(600MHz)=600MHz`, `clk_set_rate` returned success
- **But**: hardware still at 533 MHz — RPM firmware black box rejects higher rate votes
- BIMC timing registers S_SCMO at TrustZone locked — writes cause crash (build #342–#343, reverted)
- **Conclusion**: DDR cannot be overclocked on MSM8916

## Builds #338–#339

### DDR bw-tbl Override

- Added 5th entry `<4578>` to DDR bw-tbl, updated cpu-to-dev-map, added 5th GPU bus vector
- No effect on BIMC clock — RPM controls PLL, ignores higher rate votes

## Build #337

### Backlight Gamma Curve

- Replaced `MDSS_BRIGHT_TO_BL` macro with gamma-like curve (brighter at low brightness)
- `bl_min_lvl`: 30 → **1** (dimmer minimum backlight)

## Builds #334–#335

### Defconfig Cleanup

- Removed ~115 unnecessary config options: unused filesystems (btrfs, nilfs, jfs...), unused crypto algorithms (serpent, twofish, blowfish, camellia...), unused network protocols (DCCP, SCTP, DECnet, IPX, AppleTalk...), unused drivers (ISDN, telephony, WAN, CAN...)
- Cleaned DTS directory: kept only `cinnamon_A37f_defconfig` and 32 msm8916 DTS files
- Removed `CONFIG_MSM_FED` (faster panic)

## Build #333

### 120s Reapply: Fix Android Override of extra_free_kbytes

- **Scheduler reapply work (120s)** now also reapplies extra_free_kbytes=16384, swappiness=80, vfs_cache_pressure=50
- Fix: init writes extra_free_kbytes at ~50s via `sys.sysctl.extra_free_kbytes` property, overriding delay_trigger's write at ~39s. 120s reapply fires AFTER init so final value is ours.

## Build #332

### Kernel Defaults: Android-Proof Tunables

- **extra_free_kbytes**: default 0 → **16384** in `mm/page_alloc.c`
- **LMK 2GB tuning**: default minfree (1536,2048,4096,16384) → **(7168,8192,11776,15360,18944,22528)** pages in `lowmemorykiller.c`
- **Interactive governor defaults** in `cpufreq_interactive.c`:
  - go_hispeed_load: 85 → **75**, timer_rate: 20ms → **10ms**
  - min_sample_time: 80ms → **40ms**, target_load: 90 → **80**
  - io_is_busy: false → **true**, above_hispeed_delay: 10ms → **20ms**

### Notes
- Build #331 verified: L2 PC, GPU idle=500ms, RCU_BOOST_DELAY, io_is_busy survived Android override. extra_free_kbytes, LMK minfree, governor tunables were overwritten by power HAL/lmkd after boot.

## Build #331

### Performance Tuning: System
- **Disabled L2 Power Collapse**: wake latency 11ms → 240µs (msm8916-pm.dtsi, reduces jank after idle)
- **GPU idle timeout**: 80ms → 500ms (msm8916-gpu.dtsi, keeps GPU ready longer)
- **RCU_BOOST_DELAY**: 500ms → 100ms (defconfig, faster RCU callback processing)
- **extra_free_kbytes**: 0 → 16384 (delay_trigger, kswapd headroom against direct reclaim stalls)
- **Interactive governor tuning**: go_hispeed_load=75, min_sample_time=40ms, timer_rate=10ms, target_loads=80, io_is_busy=1, hispeed_freq=1497600
- **LMK 2GB tuning**: minfree 7168,8192,11776,15360,18944,22528 pages (moderate background killing)

## Build #330

### GPU Overclock 620 MHz
- Raised GPU from 465 MHz → 620 MHz (GPLL2/1.5, +33%)
- Added F(620000000, gpll2, 1.5) to oxili_gfx3d_465 frequency table
- Updated gcc_gfx3d_fmax: VDD_DIG_HIGH → 620 MHz
- Updated msm8916-gpu.dtsi: pwrlevel@0 and speed-config@2 both at 620 MHz

## Builds #328-#329

### CPU Overclock 1.5 GHz (1.4976 GHz, Corner 10)
- Raised corner 10 from 1.4 GHz → 1.4976 GHz (PLL L=78, 78 × 19.2 MHz)
- Added F_APCS_PLL entry (1497600000, L=78) to apcs_pll_freq table
- PLL fmax = 1.9 GHz at nominal voltage → safe margin
- Updated speed0-bin-v0, speed0-bin-v1, speed2-bin-v1 tables
- Updated cpufreq-table, cpu-to-dev-map with 1497600 kHz
- Verified stable: stress test held 1.5 GHz, no CPR errors

## Builds #325-#327

### DT2W Fix & Auto-Enable
- FIX: DT2W toggle wasn't applying immediately because `is_suspended` guard blocked I2C writes when screen on (build #325)
- FIX: DT2W reported KEY_F4 (143) which Android maps to WAKEUP but doesn't handle → changed to KEY_POWER (116) so Android wakes the screen (build #326)
- Auto-enable DT2W at boot 30s after boot via Cinnamon_Active Step 4 (build #327)

## Builds #319-#324

### CPU Overclock 1.3632 GHz (Corner 9)
- Added corner 9 to OPPO A37 regulator (msm8916-regulator-15399.dtsi)
- Extended cpr-corner-map, frequency-map, speed-bin-max-corners
- 1.3632 GHz from speed0-bin tables with bus bandwidth scaling
- Verified stable: 30s stress test, 45→53°C, no throttle, no CPR errors

### GPU Overclock 465 MHz + Efuse Bypass
- Raised pwrlevel@0 from 400 MHz → 465 MHz with bus bandwidth bump
- FIX: Efuse binning bypass — GCC clock driver forced 465 MHz table regardless of QFPROM (retail units locked to 400 MHz table)
- Verified: gpu_available_frequencies = 465 / 310 / 200 MHz
- GPU reaches 465 MHz under load via msm-adreno-tz governor

### GPU Governor Tuning (msm-adreno-tz)
- Reduced CEILING threshold from 50ms → 30ms (ramps to 465 MHz after ~2 frames instead of ~3)
- Event-driven (no polling): triggered on frame retire/idle → power efficient
- Touch boost: GPU wake 100ms on touch via adreno_input_work
- TrustZone algorithm handles frequency deltas securely

### Upgraded CCompress: Advanced Density & Low-Latency Execution
- **4-Bit Nibble Header System:** Upgraded from simple toggles to a high-density 4-bit metadata header (2 blocks per byte). Encodes 16 distinct compression states per block with minimal overhead.
- **4-Block Ring Buffer Recognition:** Rolling window tracking four most recent data patterns. Boosts match rates in Android's periodic memory structures.
- **Multi-Tier Delta Compression:** Specialized modes for 1, 2, and 3-byte differences (CINNAMON_MODE_DELTA1/2/3). Compresses "almost identical" blocks instead of falling back to raw data.
- **ARM64 NEON Hardware Acceleration:** Hand-optimized inline assembly for ARM64. Processes zero-checks, block matching, byte-diff counting using 128-bit SIMD.
- **Burst Run-Length Encoding:** ZERO_RUN (up to 65535 blocks) and REPEAT_BYTE_RUN modes collapse large memory buffers with near-zero latency.
- **Latency Mitigation:** Software prefetching via __builtin_prefetch; batched atomic updates reducing bus contention.

### CIO: Intelligence & Precision Architecture (Rewrite)
- **6-Tier Priority-Based Queuing:** Critical/Metadata > Read FG > Read BG > Write FG > Write BG > TRIM. Eliminates UI lockups (ANRs) and micro-stutters.
- **O(1) High-Performance Merging:** Limits merge depth to 8 entries per queue. Reduces CPU overhead and context switching.
- **Advanced FG/BG Balancing Engine:** Strict FG_STARVE_LIMIT with 16:1 dispatch ratio. Prevents background services from hijacking I/O bus.
- **eMMC-Specific Sequential Write Boost:** Intelligent detection for sequential write patterns maximizing eMMC cache efficiency.
- **Zero-Alloc Bit-Packed Metadata:** Bitwise operations embedding timestamps and priority into elv.priv pointer. Zero memory fragmentation, SMP safe.
- **Adaptive Latency-Aware Throttling:** Monitors disk health; throttles background batch sizes when latency exceeds 150ms to protect 50ms UI target.
- **FIX:** Bounded retry loop in cinnamon_set_pressure (was infinite with queue_lock held)
- **FIX:** CINNAMON_WRITE_EXPIRE corrected from jiffies to milliseconds
- **FIX:** Missing CRITICAL and TRIM/ERASE merge checks added

### frandom Driver
- ISAAC PRNG based, 24 MB/s throughput (vs urandom 2.8 MB/s, ~8.6x faster)
- /dev/frandom: fast PRNG for high-throughput random operations
- /dev/erandom: entropy-backed via get_random_bytes (get_random_bytes)
- Built as CONFIG_FRANDOM=y, chmod 644 at late-init

### Cinnamon_Active Scheduler Stability
- FIX: init.target.rc overrode Cinnamon_Active with "noop" on sys.boot_completed=1
- Added cinnamon_scheduler_reapply_work at 150s to re-apply after boot_completed
- Also sets sys.io.scheduler + persist.sys.io.scheduler properties
- Confirmed active after boot

### Other Improvements
- Removed CSleep to fix kernel power management conflicts
- WireGuard support: native kernel integration for faster, more stable VPN
- Native in-kernel root support for 'su' binary
- FIX: AnyKernel3 QCDT flash warning — CWD path corrected for AK3 environment
- FIX: Stale dtb file removed from zip (zip -r doesn't remove missing entries)

## Pending Ideas
- USB Fast Charge
- OverlayFS for Docker in chroot
- Sound Control (faux, increase max volume)
- ZRAM 1GB + lz4/zstd
- Input boost duration 40ms→80ms
