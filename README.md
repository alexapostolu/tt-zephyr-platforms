# TT-ZEPHYR-PLATFORMS

[![Build](https://github.com/tenstorrent/tt-zephyr-platforms/actions/workflows/build-fw.yml/badge.svg?branch=main)](https://github.com/tenstorrent/tt-zephyr-platforms/actions/workflows/build-fw.yml)
[![Run Unit Tests](https://github.com/tenstorrent/tt-zephyr-platforms/actions/workflows/run-unit-tests.yml/badge.svg?branch=main)](https://github.com/tenstorrent/tt-zephyr-platforms/actions/workflows/run-unit-tests.yml)
[![HW Smoke](https://github.com/tenstorrent/tt-zephyr-platforms/actions/workflows/hardware-smoke.yml/badge.svg?branch=main)](https://github.com/tenstorrent/tt-zephyr-platforms/actions/workflows/hardware-smoke.yml)
[![HW Soak](https://github.com/tenstorrent/tt-zephyr-platforms/actions/workflows/hardware-long.yml/badge.svg?branch=main)](https://github.com/tenstorrent/tt-zephyr-platforms/actions/workflows/hardware-long.yml)

Welcome to TT-Zephyr-Platforms!

This is the Zephyr firmware repository for [Tenstorrent](https://tenstorrent.com) AI ULC.

![Zephyr Shell on Blackhole](./doc/img/shell.gif)

## Getting Started

For those completely new to Zephyr, please refer to the
[Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).

The remainder of these instructions assume that system requirements, a Python virtual environment,
all Python dependencies, and the Zephyr SDK are already installed and activated.

### Check-out Sources

```shell
# Create a west workspace
west init -m https://github.com/tenstorrent/tt-zephyr-platforms ~/tt-zephyr-platforms-work
cd ~/tt-zephyr-platforms-work

# Fetch Zephyr modules
west update

# Install Python packages
west packages pip --install

# Verify binary blobs
west blobs fetch

# Apply local patches
west patch apply

# Set up Zephyr environment
source zephyr/zephyr-env.sh

# Enter the module
cd tt-zephyr-platforms
```

### Build & Flash SMC FW

> [!NOTE]
> Please replace `p100a` with the appropriate board revision for subsequent steps.
> E.g. `p100a`, `p150a`, `p150b`, `p150c`, etc.

**Build and flash targets with `west`**

```shell
# Build tt-console
make -j -C scripts/tooling OUTDIR=/tmp tt-console

# Build and flash firmware
west build --sysbuild -p -b tt_blackhole@p100a/tt_blackhole/smc app/smc -- -DCONFIG_SHELL=y
west flash -r tt_flash --force

# Reset the board and rescan the PCIe bus
tt-smi -r
./scripts/rescan-pcie.sh

# Interact via tt-console
/tmp/tt-console
```

### Build and Flash DMC FW

> [!NOTE]
> When building SMC firmware with `--sysbuild` (as shown above) it is not necessary to build and
> flash DMC firmware separately, since the `tt_flash` runner also flashes the DMC application.
> Developers should only build DMC firmware with `--sysbuild` in the event that they must perform
> an update to MCUBoot. Updating MCUBoot is not required or recommended for any end user.

**Build, flash, and view output from the target with `west`**
```shell
# Build DMC firmware
west build -b tt_blackhole@p100a/tt_blackhole/dmc app/dmc

# Flash mcuboot and the app
west flash

# Open RTT viewer
west rtt
```

Console output should appear as shown below.
```shell
*** Booting MCUboot v2.1.0-rc1-389-g4eba8087fa60 ***
*** Using Zephyr OS build v4.2.0-rc3 ***
I: Starting bootloader
I: Primary image: magic=good, swap_type=0x2, copy_done=0x1, image_ok=0x1
I: Secondary image: magic=unset, swap_type=0x1, copy_done=0x3, image_ok=0x3
I: Boot source: none
I: Image index: 0, Swap type: none
I: Bootloader chainload address offset: 0xc000
I: Image version: v0.9.99
I: Jumping to the first image slot
         .:.                 .:
      .:-----:..             :+++-.
   .:------------:.          :++++++=:
 :------------------:..      :+++++++++
 :----------------------:.   :+++++++++
 :-------------------------:.:+++++++++
 :--------:  .:-----------:. :+++++++++
 :--------:     .:----:.     :+++++++++
 .:-------:         .        :++++++++-
    .:----:                  :++++=:.
        .::                  :+=:
          .:.               ::
          .===-:        .-===-
          .=======:. :-======-
          .==================-
          .==================-
           ==================:
            :-==========-:.
                .:====-.

*** Booting tt_blackhole with Zephyr OS v4.2.0-rc3 ***
*** TT_GIT_VERSION v18.6.0-78-gf104f347ff0f ***
*** SDK_VERSION zephyr sdk 0.17.2 ***
DMFW VERSION 0.9.99
```

### Run Tests

**Build and run tests on SMC with `twister`**

> [!NOTE]
> Users may be required to patch their OpenOCD binaries to support Segger's RTT on RISC-V and ARC
> architectures. For more information, please see
> [this PR](https://github.com/zephyrproject-rtos/openocd/pull/66).

```shell
twister -i -p tt_blackhole@p100a/tt_blackhole/smc --device-testing --west-flash \
  --device-serial-pty rtt --west-runner /opt/tenstorrent/bin/openocd-rtt \
  -s samples/hello_world/sample.basic.helloworld.rtt
```

**Build and run tests on DMC with `twister`**

```shell
twister -i -p tt_blackhole@p100a/tt_blackhole/dmc --device-testing --west-flash \
  --device-serial-pty rtt --west-runner openocd \
  -s samples/hello_world/sample.basic.helloworld.rtt
```

## Enable Git Hooks for Development

To add git hooks to check your commits and branch prior to pushing to insure
they do not have any formatting or compliance issues, you can run

```shell
tt-zephyr-platforms/scripts/add-git-hooks.sh
```

## Further Reading

Learn more about `west`
[here](https://docs.zephyrproject.org/latest/develop/west/index.html).

Learn more about `twister`
[here](https://docs.zephyrproject.org/latest/develop/test/twister.html).

For more information on creating Zephyr Testsuites, visit
[this](https://docs.zephyrproject.org/latest/develop/test/ztest.html) page.

## Software License

This source code in this repository is made available under the terms of the
[Apache-2.0 software license](https://www.apache.org/licenses/LICENSE-2.0), as described in the
accompanying [LICENSE](LICENSE) file.

Additional binary artifacts are separately licensed with terms be found in
[zephyr/blobs/license.txt](zephyr/blobs/license.txt).

For the avoidance of doubt, this software assists in programming
[Tenstorrent](https://tenstorrent.com) products. However, making, using, or selling hardware,
models, or IP may require the license of rights (such as patent rights) from Tenstorrent or
others.
