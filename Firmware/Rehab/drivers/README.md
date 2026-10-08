# V1.2 external devices and SPI review record

These drivers are for `REHAB_V12` only. The original board continues linking
`Drivers/STM32/stm32_spi_arbiter.cpp`; V1.2 links the implementation in this
directory instead. Do not link both implementations. No vendor HAL source is
modified. No target has been programmed or energized during these tests.

## Integration and ownership

- Instantiate `Stm32SpiArbiter`, `Hx711`, and `Ch394` with static lifetime in normal
  SRAM, not CCM. Constructors do not configure pins or start DMA/interrupts.
- Call `spi3_arbiter.service(HAL_GetTick())` from the periodic task at 1 kHz,
  including configurations without the network module. Route the board's HAL
  SPI error callback to `on_error()`. Keep the existing completion routing.
- The application task owns `Hx711::poll(now_us)` and copies its `ForceSample`
  to the core. The caller's microsecond clock must be monotonic modulo 2^32.
- One low priority network task alone calls all `Ch394` methods. `send()` accepts
  up to the available software queue capacity; its return value must be checked.
  `receive()` copies queued bytes. Neither method accesses SPI. A stream/parser
  must be reset by its owner after `connected()` changes from true to false.
- `Ch394::init(config)` accepts static IP settings and schedules reset. It does
  not imply a detected chip or a TCP connection. `status()` supplies progress,
  last error, error/reconnect counters, version diagnostic, link and socket state.
- For a disabled module, omit its task and do not call `init()`. `deinit()` must
  run in the owning task; it leaves module pins as inputs and creates no EXTI.
  The shared SPI bus belongs to the board/DRV8301 and is not disabled by network
  deinitialization.

## SPI arbitration and cancellation

The queue admits eight waiting descriptors plus one active descriptor. Priority
0 is used by existing DRV8301/encoder callers; CH394 uses priority 3. Equal
priorities are FIFO. Each request has an enqueue-relative deadline. The default
async timeout is 10 ms; explicit timeouts are constrained to 1..1000 ms.
CH394 requests use 5 ms. Frames are counted using SPI `DataSize`: the 260-byte
limit permits 260 8-bit frames or 130 16-bit frames. CH394 sends at most 259 bytes.

DMA always uses two aligned 260-byte buffers owned by the arbiter. Caller memory
is copied only while the descriptor is exclusively owned by Launching or
Completing. In particular, an expired synchronous request never leaves a DMA
reference to its stack descriptor, result, or data buffer.

Only list/state transitions use PRIMASK. Launching excludes cancellation:
completion/error/timeout during launch is recorded, then handled by the launch
owner when HAL returns. Completing detaches a descriptor before calling its
callback, reads callback/context first, and never references that descriptor
again. The callback can release/reacquire/re-enqueue the same descriptor.
Callers using `acquire_task()` remain responsible for `release_task()` in their
completion callback. A duplicate enqueue of an active/queued descriptor is
rejected without completing the original descriptor twice.

V1.2 replaces the DMA completion/error callbacks installed by
`HAL_SPI_TransmitReceive_DMA`. Only the two SPI DMA IRQs are masked during that
installation and their prior enable states are restored. All transactions use
full duplex internally, including callers requesting only TX or RX. RX completes
the transaction; a pending TX completion is retired before another transaction
starts. DMA errors are checked before treating a simultaneous completion as
success. The replacement avoids the vendor SPI callback's 100 ms BSY wait in an
interrupt. The V1.2 BSY check has a 128-iteration limit and defers faults to task
recovery. The vendor DMA IRQ wrapper still has its original finite error-drain
loop; its measured worst-case execution time remains part of board acceptance.

If the next queued request needs a SPI configuration change or a forced recovery
reinitialization, an IRQ caller leaves it queued without removing it. The next
task-level `service()` or transfer call performs HAL/MSP reconfiguration. This
keeps GPIO/DMA setup and the HAL DMA initialization's tick-based wait out of the
SPI IRQs, which have higher priority than the motor control IRQ. An unchanged
configuration can still launch directly from completion. Deferred mode changes
can add up to one service interval to queue latency; enqueue deadlines still
include that delay.

Cancellation disables SPI DMA requests and SPI, disables the two DMA streams,
and allows at most 1024 register polling iterations while control interrupts
remain enabled. It clears DMA flags and pending SPI/DMA interrupts before
restoring their prior enable state. It does not call the HAL blocking abort
functions. If DMA EN cannot clear, the arbiter permanently quarantines itself,
keeps the peripheral IRQs disabled, fails requests, and admits no new transfer.
DMA buffers retain static lifetime even under this hardware fault. The failure
is exposed through diagnostics and subsequent DRV8301 transfers fail. Recovery
from quarantine requires a system reset after investigating the hardware fault.

`timeouts`, `errors`, `rejected`, `highwater`, and `quarantined` are available as
an atomic diagnostic snapshot. Error and timeout counters can both increment
for the same request; they are causes, not disjoint request totals.

## HX711

PC6 is DOUT, PC7 is PD_SCK, and PB1 is RATE. The driver enables DWT cycle counting
and verifies that the counter advances. It emits 24 data clocks and one gain
selection clock (channel A, gain 128). Signed 24-bit conversion is independent
of platform signed-shift behavior.

Each high pulse alone is protected by a critical section, nominally 0.75 us.
Elapsed cycles include GPIO/software overhead; a pulse exceeding 2 us invalidates
the sample and restarts settling. Low intervals allow control IRQ preemption.
An entire sample is never read under PRIMASK. GPIO edge timing must still be
measured on the actual image and board. The two high waits share a 32-iteration
fallback budget; the low wait has a 64-iteration budget. If DWT stops advancing,
the loops terminate, SCK returns low, interrupts are restored, and the conversion
is rejected. The fault-path pulse duration also requires physical verification.

The first poll begins a conservative settling interval: 50 ms at 80 SPS, 400 ms
at 10 SPS. Ready conversions are clocked out but discarded during settling.
Each accepted conversion increments the sequence and records its acquisition
time. DOUT high returns immediately and does not advance sample time/sequence;
the core handles stale-age, saturation, tare, calibration, and filtering.
After clock 25 the driver requires DOUT to return high, so a stuck-low wire cannot
publish a stream of zero-force samples. Reads are separated by at least the
configured nominal period (12.5 ms or 100 ms). A 1 ms polling task can delay an
80 SPS acquisition until the next poll; it never interpolates additional samples.

## CH394Q

The implementation was checked against the local WCH **CH394Q DS1 v1.2**
(`以太网通讯-CH394Q.pdf`), especially sections 5, 6, 7, 10.4, and 12.2.
It uses 16-bit big-endian register offset, then an 8-bit block/read-write control
byte. It does not use CH395 commands. Common block is 0x00, socket 0 is 0x08,
TX memory is 0x10, RX memory is 0x18, and write sets control bit 2.

PC2 is CS, PA4 is active-low reset, PC8 is the unused interrupt input reserved
for task polling (no EXTI8 subscription). SPI3 uses mode 0, prescaler 16:
2.625 MHz with the board's 42 MHz peripheral clock. One transaction preserves
CS across header and payload. Register accesses observe the IPv4 tRI processing
interval; <=5 MHz satisfies the datasheet's byte-spacing exception.

Reset low lasts at least 1 ms; the driver waits 40 ms after release before SPI.
The documented maximum is 19 ms following reset and 35 ms following power-up.
Factory MAC is preserved. Unused sockets receive zero buffer space before
socket 0 is enlarged to 4 KiB TX and 4 KiB RX. IP/gateway/netmask and the IPv4
mode/buffer configuration are read back. Socket 0 opens in TCP server mode on
the configured port (default 39400).

CHIPV is read twice and exposed diagnostically. Inconsistent/0x00/0xff reads are
treated as a bus sanity failure. The manual specifies its value only as 0xXX;
there is no claim that this establishes an immutable chip identity. Revision
acceptance must be checked against the fitted device during board bring-up.

Each poll performs one state-machine phase. A payload phase contains no more
than eight bounded SPI requests and moves at most 256 bytes without crossing
the 4 KiB ring boundary. Software queues are 512 bytes in each direction.
Variable 16-bit byte counts use at most three repeat comparisons; unstable
counts defer the transfer. Hardware pointers remain 16-bit counters and wrap
naturally; only memory addresses are reduced modulo 4096.

Socket commands are polled across calls with a 100 ms deadline. Data becomes
visible in the software RX queue only after RECV is acknowledged. SEND is
followed by a bounded 2 s send-completion check. Disconnection closes/reopens
the listener; transport, timeout, invalid count, and IP-conflict faults reset
the chip after a 250 ms backoff and clear both software queues. There is no
automatic restoration of an application control session.

## Software verification

The tests compile these **actual driver source files**, not a second driver
implementation. HAL/GPIO/SPI register substitutes supply deterministic device
behavior, time, failure injection, and DMA completion.

```powershell
g++ -std=gnu++17 -Wall -Wextra -Werror -DREHAB_V12 `
  -ITests/rehab/driver_mocks -IDrivers/STM32 -I. `
  Tests/rehab/driver_model_tests.cpp Rehab/drivers/spi_arbiter.cpp `
  Rehab/drivers/hx711.cpp Rehab/drivers/ch394.cpp -o build/rehab_driver_tests.exe
& ./build/rehab_driver_tests.exe
```

Current result: **730 checks passed**. Scenarios include signed conversion,
ring crossing and pointer overflow, 16-bit SPI frame sizing, queue limits and
priority, startup failure/recovery, active and queued timeout, error during
launch, simultaneous DMA error/completion, bounded BSY failure, late completion
after stack-request retirement, callback reentry and descriptor reuse, RX-only
transfer, millisecond wrap, HX711 10/80 SPS cadence, settling, stuck-high,
stuck-low, stopped DWT and timing rejection, IRQ-enqueued mode changes, deferred
HAL reinitialization after faults, and network
initialization/readback, TCP receive/send ring wrap, relisten, bad count, and
socket command timeout. The host compiler treats warnings as errors.

Not established by the model: physical GPIO/SPI timing, interrupt deadlines,
HAL register accuracy, DMA bus faults with a permanently stuck EN bit, stack
high-water marks, packet throughput, electrical compatibility, or operation on
the user's board. Those remain explicit hardware acceptance items.
