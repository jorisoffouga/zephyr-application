# mcumgr_custom: firmware update + custom MCUmgr groups

A signed MCUboot application for the STM32F4DISCOVERY + MikroE STM32F4
Discovery Shield. One UART carries MCUmgr SMP for:

* **firmware update**: the standard image group (upload, test, confirm) and
  OS group (echo, reset, info), with MCUboot test / confirm / rollback;
* **custom groups**: the application's own commands, from group 64
  (`MGMT_GROUP_ID_PERUSER`), IDs in `include/app_mgmt.h`:
  * 64 `led`: the 22 LEDs (board, MCP23S08 expander, bargraph) on, off,
    blinking, and the application version;
  * 65 `bargraph`: BarGraph click level / mask / PWM brightness;
  * 66 `airq`: Air Quality 3 click eCO2 and TVOC;
  * 67 `pattern`: LED patterns run by the firmware (K2000 scanner,
    chase, fill, alternate, counter) on any LED bank.

To add a feature, add a group (next ID: 68) or a command to an
existing group. See "Adding a group or a command" below.

```
 Linux host                                    STM32F4DISCOVERY + MikroE shield
 ┌──────────────────────────────┐  USB-UART   ┌──────────────────────────────────────┐
 │ scripts/smp_client.py        │  FT232RL    │ MCUmgr UART transport (USART2)       │
 │   led / bargraph / airq / …  │ ─── SMP ──▶ │  ├ group 0  os        echo, reset    │
 │   image upload / list / test │ /dev/ttyUSB0│  ├ group 1  img       upload, state  │
 │ swupdate (mcumgr handler)    │             │  ├ group 64 led       ── board LEDs  │
 └──────────────────────────────┘             │  │                    ── SPI2 MCP23S08 (mikroBUS 2)
                                              │  ├ group 65 bargraph  ── SPI3 74HC595 + TIM3 PWM (mikroBUS 3)
                                              │  ├ group 66 airq      ── I2C1 CCS811 (mikroBUS 1)
                                              │  └ group 67 pattern   ── LED patterns, any bank
                                              │ MCUboot: slot0 / slot1 / scratch     │
                                              └──────────────────────────────────────┘
```

The custom protocol is a set of MCUmgr groups, not a second protocol on
the UART. It uses the same framing, CRC, sequence numbers and error reporting
as the update. Any SMP client can talk to it, and it cannot interfere with
an upload in progress: requests are handled one at a time.

## Hardware

* STM32F4DISCOVERY (STM32F407G-DISC1) on the MikroE STM32F4 Discovery
  Shield (MIKROE-1481).
* **ST-LINK mini-B** (on the Discovery): power and flashing.
* **Shield mini-B** (FT232RL): SMP on USART2 (PA2 TX, PA3 RX), seen as
  `/dev/ttyUSB0`. Do not use a click board that needs UART2 at the same
  time.
* **Discovery micro-USB OTG (CN5)**: console, printk and Zephyr logs over
  USB CDC ACM (`/dev/ttyACM*`, product "mcumgr_custom log", VID:PID
  2fe3:0004, the Zephyr test IDs). It appears about 1 s after each
  reset. Without it everything else works; the logs are dropped.
* LEDs: `led0`..`led3` aliases of the board = green LD4, orange LD3,
  red LD5, blue LD6.

### Click boards

| mikroBUS | click board                    | bus                          | other pins |
|----------|--------------------------------|------------------------------|------------|
| 1        | Air Quality 3 (CCS811)         | I2C1 PB8 SCL / PB7 SDA, 0x5a | RST PE10, WAKE = CS PB12, INT PE7 |
| 2        | MCP23S08 expander, 8 LEDs active low | SPI2 PB13/14/15, CS PE14 | (RST PE12, INT PE8 not used) |
| 3        | BarGraph click (MIKROE-1423)   | SPI3 PB3/4/5, CS PE5 = latch | RST PE4 = /MR, PWM PB1 TIM3_CH4 = enable|

The full mikroBUS pinout of the shield, from its schematic, is in
`boards/stm32f4_disco.overlay`. Notes:

* The shield puts I2C1 on PB8/PB7. The Discovery uses PB6/PB9 for its
  CS43L22 audio codec, which the overlay disables.
* The MCP23S08 is addressed with A1/A0 = 0 (the Zephyr driver sends
  opcode 0x40 with HAEN off, so it works for any A1/A0 strap). Wire
  mikroBUS 2 RST to the expander and uncomment `reset-gpios` if needed.
* Unchecked assumptions about the hardware (the overlay says where to change them):
  * the BarGraph segment n is on 74HC595 output n and lit when high;
  * the expander LEDs are on GP0..GP7, lit when low;
  * the Air Quality 3 ADDR jumper is on 0x5a.
* A missing click board does not fail the image self-test: its LEDs
  answer `GPIO` / `NO_DEVICE` errors, the board LEDs still work.

udev rules and the `dialout` group: see `../mcumgr_update/README.md`.

## The LED protocol (group 64)

Defined in `include/led_mgmt.h`, implemented in `src/led_mgmt.c`
(the MCUmgr group) and `src/led_ctrl.c` (GPIOs and blinking).

| index  | LEDs                               | name         |
|--------|------------------------------------|--------------|
| 0..3   | board: green, orange, red, blue    | `User LD4` … |
| 4..11  | MCP23S08 GP0..GP7 (mikroBUS 2)     | `exp0`..`exp7` |
| 12..21 | BarGraph segments (mikroBUS 3)     | `bar0`..`bar9` |

The blinking runs from a work queue (`led_wq`), not from timer ISRs: the
expander and bargraph LEDs are SPI writes, which cannot run in an ISR.

| op    | command   | request                                                | response |
|-------|-----------|--------------------------------------------------------|----------|
| read  | 0 `state` | `{}`                                                   | `{"leds": [{"led": 0, "name": "User LD4", "mode": "blink", "period_ms": 1000}, …]}` |
| write | 0 `state` | `{"led": 2, "mode": "on" \| "off" \| "blink", "period_ms": 100}` | `{"led": 2, "name": "User LD5", "mode": "blink", "period_ms": 100}` |
| read  | 1 `version` | `{}`                                                 | `{"protocol": 2, "image": "2.0.0+0", "build": "fafb169561d8", "leds": 22}` |

* `version`: `protocol` is `LED_MGMT_PROTOCOL_VERSION`, raised when a
  command is added or changed, so a host can check what the firmware
  supports. `image` is the version signed in the MCUboot header (the same
  as `image list`). `build` is the `git describe` of the application.

* `led` defaults to 0. `period_ms` is the toggle interval (on for
  `period_ms`, off for `period_ms`), 20 to 10000, used only for `blink`.
* A malformed request (no `mode`, wrong CBOR type) gets
  `MGMT_ERR_EINVAL`. A well-formed but invalid one gets a group error,
  `{"err": {"group": 64, "rc": N}}` in SMP v2: 2 `INVALID_LED`,
  3 `INVALID_MODE`, 4 `INVALID_PERIOD`, 5 `GPIO`. SMP v1 clients get the
  `rc` mapped by `led_mgmt_translate_error()`.
* The LED state is in RAM. After a reset, LED 0 blinks at
  `CONFIG_APP_BOOT_BLINK_MS` and the others are off.
* `led get` answers about 1 KiB of CBOR, hence
  `CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE` / `_UART_MTU` = 1536 in `prj.conf`.

## The bargraph protocol (group 65)

`include/bargraph_mgmt.h`, `src/bargraph_mgmt.c`.

| op    | command   | request                                          | response |
|-------|-----------|--------------------------------------------------|----------|
| read  | 0 `state` | `{}`                                             | `{"mask": 7, "segments": 10, "brightness": 100}` |
| write | 0 `state` | `{"level": 3}` or `{"mask": 0x155}`, and/or `{"brightness": 40}` | as read |

* `level` N lights segments 0..N-1; `mask` bit n is segment n; not both.
  `brightness` 0..100 % is the PWM duty on the PWM pin (500 µs period,
  high = segments on; polarity in the overlay).
* The segments are LEDs 12..21 of the `led` group: one state for both
  groups. A segment can blink from the `led` group; it counts as on in
  `mask`.
* Errors: 2 `INVALID_LEVEL`, 3 `INVALID_MASK`, 4 `INVALID_BRIGHTNESS`,
  5 `NO_DEVICE`, 6 `IO`.

## The air quality protocol (group 66)

`include/airq_mgmt.h`, `src/airq_mgmt.c`, Zephyr `ams,ccs811` sensor
driver (one measurement per second).

| op    | command     | request | response |
|-------|-------------|---------|----------|
| read  | 0 `measure` | `{}`    | `{"eco2": 412, "tvoc": 3, "fresh": true}` |

* `eco2` in ppm, `tvoc` in ppb. `fresh` is false when the sensor has no
  new result since the previous read: the values are the previous ones.
* The CCS811 gives eCO2 400 / TVOC 0 for about 20 s after power on, and
  needs a 48 h burn-in for stable values. No temperature / humidity
  compensation (the click has no such sensor).
* Errors: 2 `NO_DEVICE` (driver init failed), 3 `IO`, 4 `INIT_PENDING`
  (first seconds after boot).
* The driver has `zephyr,deferred-init`: it starts
  `CONFIG_APP_AIRQ_INIT_DELAY_MS` (2.5 s) after boot, when the USB log is
  up. A failure is logged with an I2C probe and a hint. `device_init()`
  runs once: reset the MCU to try again.
* Command 1 `diag` (read): driver state and a raw I2C probe of 0x5a and
  0x5b with nWAKE asserted (HW_ID, STATUS, ERROR_ID, app firmware
  version):

  ```sh
  $C airq --diag
  driver init: failed (-5), devicetree address 0x5a
    0x5a: no answer
    0x5b: hw_id 0x81, hw_version 0x12, status 0x10 (app valid, boot mode), …
  => CCS811 at 0x5b but the devicetree says 0x5a: fix reg in boards/stm32f4_disco.overlay
  ```

  | result | cause |
  |--------|-------|
  | no answer at both | click not seated on mikroBUS 1, power, nWAKE (PB12), I2C pull-ups |
  | answer at the other address | ADDR jumper: change `reg` (and the node name) in the overlay |
  | `hw_id` ≠ 0x81 | not a CCS811 at this address |
  | `status` bit 4 (APP_VALID) = 0 | the CCS811 has no application firmware |

## The pattern protocol (group 67)

`include/pattern.h` (engine), `src/pattern.c`, `include/pattern_mgmt.h`,
`src/pattern_mgmt.c`. The firmware runs the pattern on the LED work queue:
it keeps running without any host.

| op    | command   | request | response |
|-------|-----------|---------|----------|
| read  | 0 `state` | `{}`    | `{"patterns": ["off", "k2000", …], "banks": [{"bank": "bargraph", "leds": 10, "pattern": "k2000", "step_ms": 70, "width": 2}, …]}` |
| write | 0 `state` | `{"bank": "bargraph", "pattern": "k2000", "step_ms": 70, "width": 2}` | the bank entry |

| pattern     | effect |
|-------------|--------|
| `k2000`     | scanner going back and forth, `width` LEDs (head and tail) |
| `chase`     | running light in one direction, `width` LEDs |
| `fill`      | bar filling up then emptying |
| `alternate` | even / odd LEDs in turn |
| `counter`   | binary counter (nice on the 8 expander LEDs) |

* Banks: `board` (4), `expander` (8), `bargraph` (10), each with its own
  pattern. `step_ms` 20..5000 (default 100), `width` 1..4 (default 2).
* `"pattern": "off"` stops it and switches the bank off. Setting an LED
  of the bank (`led` group, or `bargraph` level / mask) stops the
  pattern of that bank; the bargraph brightness does not.
* Each step writes only the LEDs that change (one SPI transfer each on
  the click boards), without a log line.
* Errors: 2 `INVALID_BANK`, 3 `INVALID_PATTERN`, 4 `INVALID_STEP`.

```sh
$C pattern start bargraph k2000 -t 70 -w 2
$C pattern start expander counter -t 200
$C pattern start board chase -t 150 -w 1
$C pattern get
$C pattern stop bargraph
```

## Logs

The SMP UART carries only SMP frames, so the logs go to a second port:
USB CDC ACM on the Discovery micro-USB OTG connector (`zephyr,console` in
the overlay, `CONFIG_USB_DEVICE_STACK_NEXT` +
`CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` in `prj.conf`). Modules `app`,
`led_ctrl`, `bargraph`, `airq`, `pattern` log at the level chosen with
`CONFIG_APP_LOG_LEVEL_INF=y` (info: boot, image confirmation, missing
click boards, LED and bargraph changes) or `CONFIG_APP_LOG_LEVEL_DBG=y`
(debug: also each air quality read).

```sh
python -m serial.tools.miniterm /dev/ttyACM2 115200
```

Other ways to get logs, if the micro-USB is not usable:

* **RTT** through the ST-LINK (`CONFIG_USE_SEGGER_RTT`,
  `CONFIG_LOG_BACKEND_RTT`, read with OpenOCD `rtt server`): no extra
  cable, but OpenOCD holds the ST-LINK (stop it to flash).
* **A third UART**: USART3 on the mikroBUS 1/2 RX/TX pins (PB11/PB10) with
  a USB-UART adapter.
* **Logs in the SMP stream** (a custom log group with a RAM ring buffer,
  read by polling): one cable, but it uses SMP bandwidth and does not
  show what happens before the application answers.

## Adding a group or a command

* **Command in an existing group:** add an ID to the group's command
  enum (e.g. `enum led_mgmt_cmd`) and a handler to its
  `mgmt_handler[]`. Raise the group's protocol version.
* **New group:** copy the layout of `airq` (the smallest group). Add
  the ID to `include/app_mgmt.h`, put the protocol in
  `include/<name>_mgmt.h` and the handlers in `src/<name>_mgmt.c`,
  registered with `MCUMGR_HANDLER_DEFINE`. Add the source to
  `CMakeLists.txt` (`target_sources_ifdef`) and a
  `CONFIG_APP_<NAME>_MGMT` option to `Kconfig`. It should depend on the
  devicetree (`DT_HAS_<compat>_ENABLED`), so a board without the
  hardware builds without the group.
* **Host side:** add the IDs, a subcommand and the group's error names
  (`GROUP_ERR`) to `scripts/smp_client.py`.
* If a response nests deeper than "map → list → map", raise the
  `MCUMGR_SMP_CBOR_MIN_ENCODING_LEVEL_*` selected in `Kconfig`.

## Images

| build | conf file  | LED 0 at boot | MCUboot version | behaviour                            |
|-------|------------|---------------|-----------------|--------------------------------------|
| v1    | –          | blink 1 s     | 1.0.0 (`VERSION`) | factory image, flashed by west     |
| v2    | `v2.conf`  | blink 150 ms  | 2.0.0           | confirms itself after boot           |
| bad   | `bad.conf` | blink 50 ms   | 3.0.0           | never confirms, so it is rolled back |

Signed with MCUboot's default **debug** key: use your own
(`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`) for anything other than tests.

Flash layout (`boards/stm32f4_disco_partitions.dtsi`, shared with
MCUboot by `sysbuild.cmake`), swap using scratch:

| partition | address  | size    |
|-----------|----------|---------|
| mcuboot   | 0x000000 | 64 KiB  |
| image-0   | 0x020000 | 256 KiB |
| image-1   | 0x060000 | 256 KiB |
| scratch   | 0x0a0000 | 128 KiB |

## 1. Build and flash

```sh
cd zephyryproject
source .venv/bin/activate
APP=zephyr-application/samples/mcumgr_custom
B=build-mcumgr_custom

west build -p -b stm32f4_disco --sysbuild -d $B-v1  $APP
west build -p -b stm32f4_disco --sysbuild -d $B-v2  $APP -- -DEXTRA_CONF_FILE=v2.conf
west build -p -b stm32f4_disco --sysbuild -d $B-bad $APP -- -DEXTRA_CONF_FILE=bad.conf

# MCUboot + v1, with the OpenOCD of the Zephyr SDK (on-board ST-LINK)
west flash -d $B-v1 -r openocd \
    --openocd ~/zephyr-sdk-0.17.4/sysroots/x86_64-pokysdk-linux/usr/bin/openocd
```

Update payloads: `$B-*/mcumgr_custom/zephyr/zephyr.signed.bin`.

## 2. Test the LED protocol

`scripts/smp_client.py` needs only pyserial (in the Zephyr venv). Close
any serial terminal on `/dev/ttyUSB0` first.

```sh
C="python $APP/scripts/smp_client.py -p /dev/ttyUSB0"

$C echo hello            # hello                 (link check, OS group)
$C led get               # led 0: blink 1000 ms, led 1..3: off
$C led on 1              # orange on
$C led blink 2 -t 100    # red blinks, 100 ms
$C led off 0             # green off
$C led get

# errors
$C led blink 3 -t 5      # error: group 64 error 4 (INVALID_PERIOD)
$C led on 7              # error: group 64 error 2 (INVALID_LED)
```

`-v` prints every decoded response and the non-SMP lines (MCUboot log).

### Click boards

```sh
$C led get               # 22 LEDs: User LD3..6, exp0..7, bar0..9
$C led on 4              # exp0 on (expander pin low)
$C led blink 11 -t 200   # exp7 blinks
$C led blink 21 -t 100   # bar9 blinks

$C bargraph level 7      # [#######...] segments 0..6
$C bargraph mask 0x155 -b 30
$C bargraph brightness 100
$C bargraph get

$C airq                  # eCO2   412 ppm   TVOC     1 ppb
$C airq -w 2             # every 2 s, Ctrl-C to stop
```

Check on the board: `led on 4` lights the first expander LED,
`bargraph level 1` the first bargraph segment. If not, fix the
assumptions in `boards/stm32f4_disco.overlay` (see "Click boards").

### Versions

```sh
$C image list            # MCUboot version and hash of each slot (image group)
$C version               # custom group: protocol 1, image 1.0.0+0, build <git>, leds 4
$C info                  # OS group, uname -a like: Zephyr version, build date, board
$C info -f rv            # kernel release and version only
$C bootloader            # MCUboot, mode 1 (swap using scratch)
```

`info` and `bootloader` are standard OS group commands
(`CONFIG_MCUMGR_GRP_OS_INFO`, `CONFIG_MCUMGR_GRP_OS_BOOTLOADER_INFO` in
`prj.conf`). Any MCUmgr client can use them.
`CONFIG_MCUMGR_GRP_OS_INFO_BUILD_DATE_TIME` embeds the build date, which
makes the build non-reproducible. Remove it if you need reproducible
builds.

## 3. Test the firmware update

With the Python client:

```sh
$C image list
$C image upload $B-v2/mcumgr_custom/zephyr/zephyr.signed.bin --test --reset
$C image list            # slot 0: 2.0.0 active confirmed
$C led get               # led 0: blink 150 ms

# rollback: the bad image runs once unconfirmed, the next reset reverts it
$C image upload $B-bad/mcumgr_custom/zephyr/zephyr.signed.bin --test --reset
$C image list            # slot 0: 3.0.0 active (not confirmed)
$C reset
$C image list            # slot 0: 2.0.0 active confirmed
```

`--test` marks slot 1 for test, `--reset` resets the MCU and waits until
the application answers again. `image test <hash>` and
`image confirm [<hash>]` do the same steps by hand.

With SWUpdate and its `mcumgr` handler (build it and its tools as in
`../mcumgr_update/README.md`; the `.swu` scripts work for any signed
image):

```sh
S=zephyr-application/samples/mcumgr_update/swupdate
$S/mkswu.sh $B-v2/mcumgr_custom/zephyr/zephyr.signed.bin custom-v2.swu /dev/ttyUSB0 mcu  2.0.0
$S/mkswu.sh $B-v1/mcumgr_custom/zephyr/zephyr.signed.bin custom-v1.swu /dev/ttyUSB0 host 1.0.0
../swupdate/build/swupdate -v -i custom-v2.swu
```

The LED group stays usable between updates: `$C led ...` then
`swupdate ...` then `$C led get`.

## Timings measured on the board

| step                                             | time          |
|--------------------------------------------------|---------------|
| boot (MCUboot, no swap) until SMP answers        | about 1.3 s   |
| upload of a 35 KiB image, 128-byte chunks        | about 15 s    |
| reset with swap (scratch, 128 KiB sectors)       | about 18 s    |

Requests sent during boot or swap are lost (MCUboot has no SMP here).
The client retries on timeout, and `reset` waits up to 120 s. For faster
uploads, raise `CONFIG_MCUMGR_TRANSPORT_UART_MTU` and `--chunk` together.
