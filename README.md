# minisforum-platform

[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue.svg)](LICENSE)
[![Kernel: 6.6+](https://img.shields.io/badge/Kernel-6.6%2B-green.svg)](https://kernel.org)
[![Platform: AMD Zen 4/5](https://img.shields.io/badge/Architecture-AMD%20Phoenix%20%2F%20Hawk%20Point-orange.svg)]()

A modular Linux kernel platform and hardware monitoring driver for Minisforum Mini-PCs (UM790 Pro, UM780 XTX, UM880 Pro, UM890 Pro, and related Venus / EliteMini series).

---

## ✨ Features

* **⚡ Native ACPI Platform Profile:** Seamlessly integrates with `/sys/firmware/acpi/platform_profile` (`low-power`, `balanced`, `performance`).
* **🎯 3-Tier Asymmetric CPPC QoS:** Automatically queries AMD CPPC Preferred Cores (`highest_perf`) and assigns hierarchical Frequency QoS limits and EPP hints across **Gold**, **Silver**, and **Standard** cores. Eliminates desktop micro-stutter while keeping multi-core workloads cool and silent.
* **❄️ In-Kernel Dynamic Thermal Governor (Zero-Daemon):** Directly drives the Embedded Controller (EC) using an Exponential Moving Average (EMA) of composite system temperatures. Provides an inaudible ~550–600 RPM idle floor, smooth stepped rate-limiting (+100 RPM/sec), and cooldown hysteresis.
* **📊 Hardware Monitoring (`hwmon`):** Exposes CPU, Motherboard, and VRM temperature sensors, dual fan tachometers, and manual PWM overrides (`pwm1` / `pwm1_enable`).
* **🛡️ Multi-Model DMI Quirks:** Automatic motherboard detection with fallback support via `force_load=1`.

---

## 🖥️ Compatibility

| Model | Board Model | APU / Processor | Status |
| :--- | :--- | :--- | :--- |
| **Minisforum UM790 Pro** | `F7BSC` | AMD Ryzen 9 7940HS | Verified |
| **Minisforum UM780 XTX** | `F7BSD` | AMD Ryzen 7 7840HS | Supported |
| **Minisforum UM880 Pro** | `F7BSI` | AMD Ryzen 7 8845HS | Supported |
| **Minisforum UM890 Pro** | `F7BSW` | AMD Ryzen 9 8945HS | Supported |
| **Other AMD UM-Series** | *F7BSx* | Phoenix / Hawk Point | Experimental (`force_load=1`) |

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

### 1. Platform Profiles & CPU Scheduling

Switch profiles natively via sysfs or desktop power managers:

```bash
cat /sys/firmware/acpi/platform_profile_choices  # [low-power balanced performance]
echo "balanced" | sudo tee /sys/firmware/acpi/platform_profile
```

#### 3-Tier CPPC QoS Architecture

| Profile | Tier | Cores (Thread) | EPP Hint | Freq QoS Range | Characteristics |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`balanced`** | 🥇 Gold<br>🥈 Silver<br>🥉 Standard | Core 1, 5<br>Core 6, 7<br>Core 0, 2, 3, 4 | `performance`<br>`balance_performance`<br>`default` | 2.40 GHz – 5.26 GHz<br>2.00 GHz – 4.40 GHz<br>1.75 GHz – 3.60 GHz | Instant desktop UI responsiveness; silent ~600 RPM idle fan; standard cores stay within the Zen 4 efficiency sweet spot. |
| **`performance`** | 🥇 Gold<br>🥈 Silver<br>🥉 Standard | Core 1, 5<br>Core 6, 7<br>Core 0, 2, 3, 4 | `performance`<br>`performance`<br>`performance` | 2.80 GHz – 5.26 GHz<br>2.40 GHz – 4.80 GHz<br>2.20 GHz – 4.30 GHz | Maximum gaming throughput; capping standard cores at 4.30 GHz avoids voltage runaway, keeping temperatures ~72–76°C. |
| **`low-power`** | 🥇 Gold<br>🥈 Silver<br>🥉 Standard | Core 1, 5<br>Core 6, 7<br>Core 0, 2, 3, 4 | `balance_performance`<br>`balance_power`<br>`power` | 1.75 GHz – 3.20 GHz<br>1.40 GHz – 2.60 GHz<br>1.20 GHz – 2.00 GHz | 35W TDP Eco profile; zero jitter on 120Hz display and video playback; inaudible 0 dB fan. |

#### Desktop UI Integration (GNOME / KDE)
For seamless integration with desktop Power Mode sliders, **`tuned-ppd`** is recommended over legacy `power-profiles-daemon`:
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
CPU Fan:      598 RPM
System Fan:     0 RPM
CPU EC:       +42.0°C  
Motherboard:  +40.0°C  
VRM:          +39.0°C  
pwm1:             12%
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

## ⚙️ Module Parameters

Configure options at runtime or in `/etc/modprobe.d/minisforum_platform.conf`:

| Parameter | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `manage_cpufreq` | bool | `true` | Enable in-kernel Frequency QoS and CPPC coordination. |
| `force_load` | bool | `false` | Bypass DMI quirk checks for experimental/untested boards. |
| `freq_low_power_min` | uint | `1754353` | Floor frequency (kHz) for low-power profile. |
| `freq_low_power_max` | uint | `3200000` | Ceiling frequency (kHz) for low-power profile. |
| `freq_balanced_min` | uint | `1754353` | Baseline floor frequency (kHz) for balanced profile. |
| `freq_performance_min` | uint | `2631530` | Baseline floor frequency (kHz) for performance profile. |

---

## 🗑️ Uninstallation

```bash
sudo make dkms-uninstall
sudo rm -f /etc/modules-load.d/minisforum_platform.conf
```

---

## 📄 License

GPL-2.0-or-later © Dwi Mulia Mokoginta
