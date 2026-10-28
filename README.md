# minisforum-platform

[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue.svg)](LICENSE)
[![Kernel: 6.6+](https://img.shields.io/badge/Kernel-6.6%2B-green.svg)](https://kernel.org)
[![Platform: AMD Zen 4/5](https://img.shields.io/badge/Architecture-AMD%20Phoenix%20%2F%20Hawk%20Point-orange.svg)]()

A modular Linux kernel platform and hardware monitoring driver for Minisforum Mini-PCs (UM790 Pro, UM780 XTX, UM880 Pro, UM890 Pro, and related Venus / EliteMini series).

---

## ✨ Features

* **⚡ Native ACPI Platform Profile:** Seamlessly integrates with `/sys/firmware/acpi/platform_profile` (`low-power`, `balanced`, `performance`), directing hardware power states straight to the Embedded Controller (EC).
* **❄️ In-Kernel Dynamic Thermal Governor (Zero-Daemon):** Directly drives the Embedded Controller (EC) using an Exponential Moving Average (EMA) of composite system temperatures. Provides an inaudible ~600–940 RPM idle floor, smooth stepped rate-limiting (+100 RPM/sec), and cooldown hysteresis.
* **📊 Hardware Monitoring (`hwmon`):** Exposes CPU, Motherboard, and VRM temperature sensors, dual fan tachometers, and manual PWM overrides (`pwm1` / `pwm1_enable`).
* **🛡️ Zero-Parameter DMI Auto-Detection:** Automatically matches hardware across UM-series models without requiring custom module parameters or intrusive frequency overrides.

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
> There are no module parameters, so a board either matches a DMI entry or the
> driver does not bind at all.

> [!NOTE]
> Intel-based models (MS-01, NPB series) and dual-blower dGPU models (HX99G, Neptune series) use entirely different EC architectures and are not supported.

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

| Profile | Characteristics |
| :--- | :--- |
| **`balanced`** | Default profile. Elastic CPU boost up to 5.26 GHz with standard `amd-pstate` scaling; silent ~940 RPM idle fan floor. |
| **`performance`** | Maximum throughput for gaming and sustained compute workloads; ~1000 RPM base fan floor with aggressive cooling ramp. |
| **`low-power`** | Eco / quiet profile; inaudible ~600 RPM fan floor with power-optimized thermal targets. |

#### Desktop UI Integration (GNOME / KDE)
For seamless integration with desktop Power Mode sliders, **`tuned-ppd`** is recommended:
```bash
sudo pacman -S tuned tuned-ppd
sudo systemctl enable --now tuned tuned-ppd
```

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

#### Manual PWM Override
```bash
HWMON_DIR=$(dirname $(grep -l "minisforum" /sys/class/hwmon/hwmon*/name))

# Lock fan manually (e.g. 25% / ~1300 RPM)
echo 65 | sudo tee $HWMON_DIR/pwm1

# Restore in-kernel dynamic governor (Auto Mode)
echo 2 | sudo tee $HWMON_DIR/pwm1_enable
```
*(Hardware safety guard: The EC firmware will automatically override to 100% full speed at $\ge 94^\circ\text{C}$ regardless of manual settings).*

---

## 🗑️ Uninstallation

```bash
sudo make dkms-uninstall
sudo rm -f /etc/modules-load.d/minisforum_platform.conf
```

---

## 📄 License

GPL-2.0-or-later © Dwi Mulia Mokoginta
