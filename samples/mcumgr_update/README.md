# mcumgr_update: test the SWUpdate `mcumgr` handler

This sample is a signed MCUboot application. SWUpdate, built and run on
the Linux host, updates it over a UART with MCUmgr SMP. It tests the
`mcumgr` handler in `swupdate/handlers/mcumgr_handler.c`.

```
 Linux host                                 MCU board
 ┌───────────────────────┐   USB-UART      ┌───────────────────────────┐
 │ swupdate -i app.swu   │ ──── SMP ─────▶ │ MCUmgr UART transport     │
 │  └ handler "mcumgr"   │                 │  slot0 / slot1 (MCUboot)  │
 └───────────────────────┘                 └───────────────────────────┘
```

On the board, SMP runs on the MCUmgr UART transport
(`CONFIG_MCUMGR_TRANSPORT_UART`, the UART in `zephyr,uart-mcumgr`). The
application has no shell, console or printk, so the UART carries only SMP
frames. MCUboot still prints its boot log at reset; the handler ignores
any line that is not an SMP frame.

## Boards

| board (`-b`)    | hardware                                              | host UART      | MCUboot mode       | flash with      |
|-----------------|-------------------------------------------------------|----------------|--------------------|-----------------|
| `frdm_k64f`     | FRDM-K64F                                             | `/dev/ttyACM0` | swap using move    | `-r pyocd`      |
| `stm32f4_disco` | STM32F4DISCOVERY (STM32F407G-DISC1) + MikroE STM32F4 Discovery Shield (MIKROE-1481) | `/dev/ttyUSB0` | swap using scratch | `-r openocd`    |

The MCUboot mode is chosen per board in `Kconfig.sysbuild`, from the flash
layout:

* **frdm_k64f**: the board devicetree has the MCUboot partitions; SMP on
  uart0, the OpenSDA USB-UART (`zephyr,uart-mcumgr` set by the board).
* **stm32f4_disco**: the STM32F407VG has 16/64/128 KiB sectors, the
  partitions are in `boards/stm32f4_disco_partitions.dtsi`, used by the
  application (`boards/stm32f4_disco.overlay`) and by MCUboot
  (`sysbuild.cmake`):

  | partition | address  | size    |
  |-----------|----------|---------|
  | mcuboot   | 0x000000 | 64 KiB  |
  | (unused)  | 0x010000 | 64 KiB  |
  | image-0   | 0x020000 | 256 KiB |
  | image-1   | 0x060000 | 256 KiB |
  | scratch   | 0x0a0000 | 128 KiB |

  SMP runs on USART2 (PA2 TX, PA3 RX). On the MikroE shield, USART2 is
  wired to the FT232RL USB-UART (shield schematic: FT232RL TXD to
  PA3/UART2-RX, RXD to PA2/UART2-TX): connect the **mini-B USB connector
  of the shield** to the host (`/dev/ttyUSB0`), and the mini-B of the
  ST-LINK for power and flashing. Do not plug a click board using UART2
  in the mikroBUS sockets at the same time.

## Images

| build      | conf file  | LED blink | MCUboot version | behaviour                      |
|------------|------------|-----------|-----------------|--------------------------------|
| v1         | –          | 1 s       | 1.0.0           | factory image, flashed by west |
| v2         | `v2.conf`  | 150 ms    | 2.0.0           | confirms itself after boot     |
| bad        | `bad.conf` | 50 ms     | 3.0.0           | never confirms, so it is rolled back |

The images are signed with MCUboot's default **debug** key. Use your own
key (`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`) for anything other than tests.

## 1. Build and flash the MCU

```sh
cd zephyryproject
source .venv/bin/activate
APP=zephyr-application/samples/mcumgr_update
BOARD=frdm_k64f            # or stm32f4_disco

# v1: MCUboot + application, flashed once
west build -p -b $BOARD --sysbuild -d build-$BOARD-v1 $APP

# update images (not flashed)
west build -p -b $BOARD --sysbuild -d build-$BOARD-v2  $APP -- -DEXTRA_CONF_FILE=v2.conf
west build -p -b $BOARD --sysbuild -d build-$BOARD-bad $APP -- -DEXTRA_CONF_FILE=bad.conf
```

Flash v1 (MCUboot and the signed application):

```sh
# frdm_k64f: the default runner is LinkServer; pyOCD (in .venv) works with
# the stock OpenSDA/DAPLink firmware
west flash -d build-frdm_k64f-v1 -r pyocd

# stm32f4_disco: the default runner is STM32CubeProgrammer; OpenOCD from
# the Zephyr SDK works with the on-board ST-LINK
west flash -d build-stm32f4_disco-v1 -r openocd \
    --openocd ~/zephyr-sdk-0.17.4/sysroots/x86_64-pokysdk-linux/usr/bin/openocd
```

The update payload is `build-<board>-*/mcumgr_update/zephyr/zephyr.signed.bin`.

USB access without root (udev rule, then unplug / replug the boards):

```sh
sudo tee /etc/udev/rules.d/60-mcumgr-update.rules >/dev/null <<'EOR'
# FRDM-K64F OpenSDA (DAPLink): probe and /dev/ttyACM*
SUBSYSTEM=="usb", ATTR{idVendor}=="0d28", ATTR{idProduct}=="0204", MODE:="0666", TAG+="uaccess"
KERNEL=="ttyACM*", ATTRS{idVendor}=="0d28", ATTRS{idProduct}=="0204", MODE:="0666", TAG+="uaccess"
# STM32F4DISCOVERY ST-LINK/V2 and V2-A
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="3748", MODE:="0666", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="374b", MODE:="0666", TAG+="uaccess"
# MikroE shield FT232RL: /dev/ttyUSB*
KERNEL=="ttyUSB*", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", MODE:="0666", TAG+="uaccess"
EOR
sudo udevadm control --reload-rules && sudo udevadm trigger
```

The running image shows on the LED blink rate (green LED). It is also in
the SMP image state (`version`, `confirmed`), readable with any MCUmgr
client on the board UART.
**Close any serial terminal before running SWUpdate.** Only one program
can use the UART.

## 2. Build SWUpdate for the host

```sh
sudo apt install build-essential pkg-config libconfig-dev libjson-c-dev \
                 libubootenv-dev libssl-dev zlib1g-dev
$APP/swupdate/build-host-swupdate.sh ../swupdate ../swupdate/build-host
```

This uses `swupdate/host_defconfig`: a local install, no Lua, no web
server, the `mcumgr` handler, and sha256 checks.

## 3. Create the .swu files (swugenerator)

The `.swu` files are made with [swugenerator](https://github.com/sbabic/swugenerator)
(`pip install swugenerator`). The template `swupdate/sw-description` uses
`@@VERSION@@`, `@@DEVICE@@` and `@@CONFIRM@@` placeholders. swugenerator
takes their values from a `-c` config file, finds `zephyr.signed.bin` in
the artifactory (`-a`) and adds its `sha256`.

Direct call, with the variables in `swupdate/swugenerator.conf`:

```sh
S=$APP/swupdate
swugenerator -s $S/sw-description -c $S/swugenerator.conf \
             -a build-frdm_k64f-v2/mcumgr_update/zephyr -o v2.swu create
```

Or use the wrapper, which writes the variables for each image:

```sh
# frdm_k64f: UART=/dev/ttyACM0, stm32f4_disco: UART=/dev/ttyUSB0
UART=/dev/ttyACM0
B=build-$BOARD
#            <signed image>                                <swu>    <uart> <confirm> <version>
$S/mkswu.sh $B-v2/mcumgr_update/zephyr/zephyr.signed.bin  v2.swu  $UART  mcu   2.0.0
$S/mkswu.sh $B-bad/mcumgr_update/zephyr/zephyr.signed.bin bad.swu $UART  mcu   3.0.0
$S/mkswu.sh $B-v1/mcumgr_update/zephyr/zephyr.signed.bin  v1.swu  $UART  host  1.0.0
```

The `.swu` files are specific to a board: the images are built for it.

### Both boards in one .swu

With the two boards connected, `swupdate/sw-description-two-boards`
updates them from a single `.swu`: one `mcumgr` image per board, each on
its own UART. Both builds name their image `zephyr.signed.bin`, so the
wrapper copies them as `frdm_k64f.signed.bin` and
`stm32f4_disco.signed.bin`:

```sh
#                     <frdm_k64f image>                                     <stm32f4_disco image>                                     <swu>         <k64f uart>  <f4 uart>    <confirm> <version>
$S/mkswu-two-boards.sh build-frdm_k64f-v2/mcumgr_update/zephyr/zephyr.signed.bin build-stm32f4_disco-v2/mcumgr_update/zephyr/zephyr.signed.bin both-v2.swu /dev/ttyACM0 /dev/ttyUSB0 mcu 2.0.0
../swupdate/build-host/swupdate -v -i both-v2.swu
```

The images are installed one after the other (frdm_k64f first). Each MCU
rolls back on its own, but there is no transaction across the two: if the
stm32f4_disco update fails, SWUpdate reports an error and the frdm_k64f
keeps its new image.

To sign the `.swu` (the host SWUpdate then needs `CONFIG_SIGNED_IMAGES`), add
for example `-k RSA,priv.pem` before `create`.

The `confirm` value sets who confirms the new image:
* `mcu`: the application confirms itself after its self-test.
* `host`: SWUpdate confirms it once it is running.

## 4. Run the update

The user must have access to the UART:
`sudo usermod -aG dialout $USER`, then log out and back in.

```sh
../swupdate/build-host/swupdate -v -i v2.swu    # LED: 1 s -> 150 ms, rc 0
../swupdate/build-host/swupdate -v -i bad.swu   # LED: 50 ms, then back to 150 ms, rc 1
../swupdate/build-host/swupdate -v -i v1.swu    # back to v1, confirmed by the host
```

What the handler does:
1. reads the image state (checks that the MCU answers)
2. uploads the image to slot1 (the first chunk waits for the slot erase)
3. marks slot1 for test, then resets the MCU
4. waits for MCUboot's swap and checks that slot0 holds the new hash
5. waits for confirmation (`mcu`) or confirms the image (`host`)
6. on any error after the swap, resets the MCU so MCUboot reverts

## Without hardware: MCU emulator

SWUpdate provides `examples/mcumgr/mcumgr_emulator.py`: it emulates the
Zephyr SMP serial transport and the test, confirm and revert logic of
MCUboot on a pseudo terminal. Options simulate an image that does not
confirm itself (`--mode bad`), an image rejected by MCUboot
(`--mode reject`), a slow reset, lost or corrupted frames, and a
USB-serial bridge holding the last byte of each write (`--hold-tail`).

```sh
../swupdate/examples/mcumgr/mcumgr_emulator.py --link /tmp/ttyMCU &
$S/mkswu.sh build-frdm_k64f-v2/mcumgr_update/zephyr/zephyr.signed.bin t.swu /tmp/ttyMCU mcu
../swupdate/build-host/swupdate -v -i t.swu
```

## Tuning

* `chunk-size` (default 128) must fit `CONFIG_MCUMGR_TRANSPORT_UART_MTU`
  (default 256). Raise both together for faster uploads.
* `boot-timeout-ms` must cover MCUboot's swap time. It grows with the
  image size; with swap using scratch on the STM32F407, each 128 KiB
  sector is erased several times (about 1 to 2 s per erase), keep the
  60 s of the template.
