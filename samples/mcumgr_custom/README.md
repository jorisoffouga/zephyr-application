# mcumgr_custom: firmware update + custom MCUmgr groups

A signed MCUboot application for the STM32F4DISCOVERY + MikroE STM32F4
Discovery Shield. One UART carries MCUmgr SMP for:

* **firmware update**: the standard image group (upload, test, confirm) and
  OS group (echo, reset, info), with MCUboot test / confirm / rollback;
* **custom groups**: the application's own commands, from group 64
  (`MGMT_GROUP_ID_PERUSER`), IDs in `include/app_mgmt.h`:
  * 64 `led`: the board LEDs on, off, blinking, and the application
    version.

To add a feature, add a group (next ID: 65) or a command to an
existing group. See "Adding a group or a command" below.

```
 Linux host                                    STM32F4DISCOVERY + MikroE shield
 ┌──────────────────────────────┐  USB-UART   ┌──────────────────────────────────────┐
 │ scripts/smp_client.py        │  FT232RL    │ MCUmgr UART transport (USART2)       │
 │   led / version / info       │ ─── SMP ──▶ │  ├ group 0  os        echo, reset    │
 │   image upload / list / test │ /dev/ttyUSB0│  ├ group 1  img       upload, state  │
 │ swupdate (mcumgr handler)    │             │  └ group 64 led       ── board LEDs  │
 └──────────────────────────────┘             │                                      │
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

udev rules and the `dialout` group: see `../mcumgr_update/README.md`.

## The LED protocol (group 64)

Defined in `include/led_mgmt.h`, implemented in `src/led_mgmt.c`
(the MCUmgr group) and `src/led_ctrl.c` (GPIOs and blinking).

| index  | LEDs                               | name         |
|--------|------------------------------------|--------------|
| 0..3   | board: green, orange, red, blue    | `User LD4` … |

The blinking runs from a work queue (`led_wq`), not from timer ISRs, so
LEDs behind SPI GPIO expanders can be added (devicetree `gpio-leds`).

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

## Logs

The SMP UART carries only SMP frames, so the logs go to a second port:
USB CDC ACM on the Discovery micro-USB OTG connector (`zephyr,console` in
the overlay, `CONFIG_USB_DEVICE_STACK_NEXT` +
`CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` in `prj.conf`). Modules `app`,
`led_ctrl` log at the level chosen with
`CONFIG_APP_LOG_LEVEL_INF=y` (info: boot, image confirmation, LED
changes) or `CONFIG_APP_LOG_LEVEL_DBG=y`
(debug).

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
* **New group:** copy the layout of the `led` group. Add
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
