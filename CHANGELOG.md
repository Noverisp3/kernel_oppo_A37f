# Cinnamon Kernel Changelog

## Build #333 (Current)

### 120s Reapply: Fix Android Override extra_free_kbytes

- **Scheduler reapply work (120s)** now also reapplies extra_free_kbytes=16384, swappiness=80, vfs_cache_pressure=50
- Fix: init writes extra_free_kbytes at ~50s via `sys.sysctl.extra_free_kbytes` property, overriding delay_trigger's write at ~39s. 120s reapply fires AFTER init so final value is ours.

## Build #332

### Kernel Defaults: Android-Proof Tunables

- **extra_free_kbytes**: default 0 → **16384** in `mm/page_alloc.c`
- **LMK 2GB tuning**: default minfree (1536,2048,4096,16384) → **(7168,8192,11776,15360,18944,22528)** pages trong `lowmemorykiller.c`
- **Interactive governor defaults** trong `cpufreq_interactive.c`:
  - go_hispeed_load: 85 → **75**, timer_rate: 20ms → **10ms**
  - min_sample_time: 80ms → **40ms**, target_load: 90 → **80**
  - io_is_busy: false → **true**, above_hispeed_delay: 10ms → **20ms**

### Ghi chú
- Build #331 verified: L2 PC, GPU idle=500ms, RCU_BOOST_DELAY, io_is_busy giữ được. extra_free_kbytes, LMK minfree, governor tunables bị Android power HAL/lmkd ghi đè sau boot.

## Build #331

### Performance Tuning: Hệ thống
- **Tắt L2 Power Collapse**: wake latency 11ms → 240µs (msm8916-pm.dtsi, giảm jank sau idle)
- **GPU idle timeout**: 80ms → 500ms (msm8916-gpu.dtsi, giữ GPU sẵn sàng lâu hơn)
- **RCU_BOOST_DELAY**: 500ms → 100ms (defconfig, boost RCU callbacks nhanh hơn)
- **extra_free_kbytes**: 0 → 16384 (delay_trigger, kswapd headroom chống direct reclaim stall)
- **Interactive governor tuning**: go_hispeed_load=75, min_sample_time=40ms, timer_rate=10ms, target_loads=80, io_is_busy=1, hispeed_freq=1497600
- **LMK 2GB tuning**: minfree 7168,8192,11776,15360,18944,22528 pages (giết nền vừa phải)

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
- Dynamic Fsync
- OverlayFS for Docker in chroot
