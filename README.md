# zephyr-application

A Zephyr module and west manifest: out-of-tree boards, drivers, a snippet and
samples, on **Zephyr 4.5** (`west.yml` tracks Zephyr `v4.5-branch`).

| Directory | Content |
|-----------|---------|
| `boards/arm/imx8mp_debix_model_a` | Cortex-M7 of the i.MX8M Plus DEBIX Model A (`imx8mp_debix_model_a/mimx8ml8/m7`, `.../m7/ddr`) |
| `drivers`, `dts/bindings` | NXP PCF8591 (MFD with ADC and DAC children) |
| `snippets/warp7-io-board` | WaRP7 IO board devices on I2C3: LM75A, MCP7940N, 24C08, PCF8591 (`-S warp7-io-board`) |
| `include/za` | headers shared by the drivers and samples (`m4io_proto.h`: RPMsg IO protocol) |
| `lib` | cJSON (`CONFIG_CJSON`) |
| `samples` | applications, see below |

## Get the sources

```sh
mkdir zephyryproject && cd zephyryproject
python3 -m venv .venv && source .venv/bin/activate
pip install west
west init -m https://github.com/jorisoffouga/zephyr-application.git --mr main
west update
pip install -r zephyr/scripts/requirements.txt
```

Toolchain: the Zephyr SDK, see the
[Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).

`west update` after a change of `west.yml` (Zephyr version): it moves
Zephyr and every module. Build directories made with another Zephyr version
need a pristine build (`west build -p ...`).

## Samples

| Sample | Boards | What it does |
|--------|--------|--------------|
| `hello_world` | `warp7/mcimx7d/m4`, `imx8mp_debix_model_a/mimx8ml8/m7` | Hello World on the Cortex-M core |
| `openamp_rsc_table` | `warp7/mcimx7d/m4`, `stm32mp157x_dk2/stm32mp157cxx` | OpenAMP remote with a resource table, loaded by Linux remoteproc |
| `rpmsg_echo` | `warp7/mcimx7d/m4`, `imx8mp_debix_model_a/mimx8ml8/m7` | RPMsg echo on an rpmsg-tty channel, to test remoteproc |
| `rpmsg_io` | `warp7/mcimx7d/m4` + `-S warp7-io-board` | m4io service: Linux drives the M4 IO devices over RPMsg |
| `warp7_io_board` | `warp7/mcimx7d/m4` + `-S warp7-io-board` | WaRP7 IO board devices (I2C3) driven by the Cortex-M4 |
| `mcumgr_update` | `stm32f4_disco`, `frdm_k64f` | Signed MCUboot application updated over SMP/UART by SWUpdate |
| `mcumgr_custom` | `stm32f4_disco` + MikroE shield | MCUmgr update plus custom SMP groups for click boards, web UI ([README](samples/mcumgr_custom/README.md)) |

Examples:

```sh
west build -p -b warp7/mcimx7d/m4 zephyr-application/samples/rpmsg_echo
west build -p -b imx8mp_debix_model_a/mimx8ml8/m7 zephyr-application/samples/rpmsg_echo
west build -p -b warp7/mcimx7d/m4 -S warp7-io-board zephyr-application/samples/warp7_io_board
west build -p -b stm32f4_disco --sysbuild zephyr-application/samples/mcumgr_custom
```
