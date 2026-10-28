# minisforum-platform

[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue.svg)](LICENSE)
[![Kernel: 6.6+](https://img.shields.io/badge/Kernel-6.6%2B-green.svg)](https://kernel.org)
[![Platform: AMD Zen 4/5](https://img.shields.io/badge/Architecture-AMD%20Phoenix%20%2F%20Hawk%20Point-orange.svg)]()

A modular Linux kernel platform and hardware monitoring driver for Minisforum Mini-PCs (UM790 Pro, UM780 XTX, UM880 Pro, UM890 Pro, and related Venus / EliteMini series).

---

## ✨ Features

* **⚡ Native ACPI Platform Profile:** Seamlessly integrates with `/sys/firmware/acpi/platform_profile` (`low-power`, `balanced`, `performance`), directing hardware power states straight to the Embedded Controller (EC).
* **📊 Hardware Monitoring (`hwmon`):** Exposes CPU, Motherboard, and VRM temperature sensors, dual fan tachometers, and standard PWM fan control (`pwm1` / `pwm1_enable` / `pwm1_mode`) that `fancontrol`, `pwmconfig` and CoolerControl drive directly.
* **🛡️ Zero-Parameter, Mechanism-Only:** No module parameters, no CPU frequency overrides, and no in-kernel thermal policy. Custom fan curves belong in userspace, exactly as the hwmon ABI intends.

---

## 🖥️ Compatibility

| Model | Board Model | APU / Processor | Status |
| :--- | :--- | :--- | :--- |
| **Minisforum UM790 Pro** | `F7BSC` | AMD Ryzen 9 7940HS | Verified |
| **Minisforum UM780 XTX** | `F7BSD` | AMD Ryzen 7 7840HS | Untested |
| **Minisforum UM880 Pro** | `F7BSI` | AMD Ryzen 7 8845HS | Untested |
| **Minisforum UM890 Pro** | `F7BSW` | AMD Ryzen 9 8945HS | Untested |

> [!NOTE]
> Only `F7BSC` has actually been tested. The other DMI entries assume the same
> EC register layout, so the driver binds to them, but nobody has run them.
>
> Adding a board is **not** just a DMI entry. Every register address — EC
> temperature registers, the profile selector, the XRAM fan curve and the
> tachometers — is a hardcoded `#define`. A board whose EC firmware lays them
> out differently would be driven with the wrong addresses, so it needs a
> register layout in `minisforum_board_config` first.
>
> Intel-based models (MS-01, NPB series) and dual-blower dGPU models (HX99G,
> Neptune series) use entirely different EC architectures and are not supported.

---

## 📦 Installation

### Arch Linux / CachyOS (AUR)
```bash
paru -S minisforum-platform-dkms
# or
yay -S minisforum-platform-dkms
```

### Ubuntu / Debian
Download the `.deb` package from the [Releases](https://github.com/malwareslayer/minisforum-platform/releases) page:
```bash
sudo dpkg -i minisforum-platform-dkms_*.deb
sudo apt-get install -f
```

### Manual / DKMS Build
```bash
git clone https://github.com/malwareslayer/minisforum-platform.git
cd minisforum-platform
sudo make dkms-install
sudo modprobe minisforum_platform
```

To auto-load on boot:
```bash
echo "minisforum_platform" | sudo tee /etc/modules-load.d/minisforum_platform.conf
```

---

## 🚀 Usage

### 1. Platform Profiles & CPU Coordination

Switch profiles natively via sysfs or desktop power managers:

```bash
cat /sys/firmware/acpi/platform_profile_choices  # [low-power balanced performance]
echo "balanced" | sudo tee /sys/firmware/acpi/platform_profile
```

| Profile | What it does |
| :--- | :--- |
| **`balanced`** | Default. Writes the EC's CPU power profile selector; the power envelope stays with the firmware. |
| **`performance`** | Maximum-throughput selector, for gaming and sustained compute. |
| **`low-power`** | Eco / quiet selector. |

> These profiles only move the EC selector — they do **not** touch CPU
> frequency and they do **not** touch the fan. On `amd-pstate` *active* mode
> EPP is what actually changes behaviour, and the fan belongs to userspace.
> Let `tuned` drive both; see *Userspace Fan Control* below.

#### Desktop UI Integration (GNOME / KDE)
For seamless integration with desktop Power Mode sliders, **`tuned-ppd`** is recommended:
```bash
sudo pacman -S tuned tuned-ppd
sudo systemctl enable --now tuned tuned-ppd
```
The slider then sets `platform_profile` for you, and `tuned` applies the
matching EPP on top of it.

---

### 2. Hardware Monitoring & Fan Control

View live sensor telemetry via standard `lm_sensors`:

```bash
$ sensors minisforum-isa-0000
minisforum-isa-0000
Adapter: ISA adapter
CPU Fan:      849 RPM
System Fan:     0 RPM
CPU EC:       +40.0°C  
Motherboard:  +38.0°C  
VRM:          +36.0°C  
pwm1:             22%
```

#### Userspace Fan Control (`pwm1_enable`)

The driver follows the standard hwmon ABI, so `fancontrol`, `pwmconfig` and
CoolerControl can drive the fan directly:

| Attribute | Value | Meaning |
| :--- | :--- | :--- |
| `pwm1_enable` | `0` | no control — fan forced to full speed |
| | `1` | manual — userspace owns `pwm1` |
| | `2` | **automatic (default)** — EC firmware thermal curve |
| `pwm1` | `25`–`255` | requested duty (0–100%), writable in manual mode only |
| `pwm1_mode` | `1` | PWM modulation (read-only) |

```bash
HWMON_DIR=$(dirname $(grep -l "minisforum" /sys/class/hwmon/hwmon*/name))

# Take manual control (e.g. 25% / ~1300 RPM)
echo 1   | sudo tee $HWMON_DIR/pwm1_enable
echo 65  | sudo tee $HWMON_DIR/pwm1

# Hand the fan back to the EC firmware curve
echo 2   | sudo tee $HWMON_DIR/pwm1_enable
```

Writing `pwm1` without setting `pwm1_enable=1` first switches to manual mode
automatically, so tools that write `pwm1` directly also work.

**Integration with `fancontrol` / `pwmconfig` (lm-sensors):**
```bash
sudo pwmconfig                      # detects fan1/pwm1 and generates a config
sudo systemctl enable --now fancontrol
```

> `fancontrol` embeds `hwmonN` in `/etc/fancontrol`, and that number shifts
> between boots when other devices add hwmon instances, so the config breaks
> with "Device path of hwmonN has changed". **CoolerControl** finds the device
> by name instead and is the simpler choice. Note also that `pwmconfig` cannot
> calibrate `MINSTOP`, because manual duty is clamped and the fan never stops.

While `pwm1_enable=1`, the driver does not touch the fan at all and profile
changes no longer affect it. In mode `2` the EC firmware owns the curve, so
`pwm1` reads `0` — use `fan1_input` for the actual speed.

> **Idle noise:** with no in-kernel thermal policy the fan simply follows the
> Minisforum factory curve, which is usually more aggressive than a hand-tuned
> one. For a quieter idle, build a curve in userspace with `fancontrol` (or
> CoolerControl) instead of expecting the kernel to invent one.

> **Safety notes**
> * Manual duty is clamped to a floor of `25` (code 5, ~450–500 RPM): a stray
>   `echo 0 > pwm1` cannot stop the CPU fan.
> * The EC firmware keeps its own watchdog and forces full speed at $\ge 94^\circ\text{C}$
>   regardless of the requested duty; that protection row is never written by
>   the driver.
> * Unloading the module restores the factory BIOS fan curve, so the fan never
>   stays stuck at a manual duty.

---

## 🗑️ Uninstallation

```bash
sudo make dkms-uninstall
sudo rm -f /etc/modules-load.d/minisforum_platform.conf
```

---

## 📄 License

GPL-2.0-or-later © Dwi Mulia Mokoginta
