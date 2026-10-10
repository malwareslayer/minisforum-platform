# minisforum-platform

A modular Linux kernel platform driver for Minisforum Mini-PCs (UM790 Pro, UM780 XTX, UM880, and other Venus / EliteMini series).

This driver bridges missing OEM platform features on Linux through a clean, modular architecture:

* **TDP Switcher (`platform_profile`):** Native Linux ACPI platform profile integration (`low-power`, `balanced`, `performance`). Works out-of-the-box with GNOME Quick Settings, KDE Plasma, and `power-profiles-daemon`.
* **Dynamic Thermal Governor & Stepped EC Writes (Zero-Daemon):** Directly controls the embedded controller's (EC) hardware with a composite system thermal average (CPU die + VRM + Motherboard) filtered via Exponential Moving Average (EMA). Features a rock-solid inaudible ~550-600 RPM idle floor, gradual rate-limited step-up, and hysteresis step-down—ensuring zero erratic cycling, zero audible jumps, and native synergy with `amd-pstate=guided` / `schedutil`.
* **Hardware Monitoring & Manual Control (`hwmon`):** Exposes CPU, Motherboard, VRM temperature sensors, dual fan RPM tachometers, and manual PWM override controls (`pwm1` / `pwm1_enable`).
* **Multi-Model Support:** Built with a DMI quirk table that automatically recognizes and adapts to various Minisforum motherboard models.

---

## Supported Hardware

| Device | Board Model | APU / Processor | Status |
| :--- | :--- | :--- | :--- |
| **Minisforum UM790 Pro** | `F7BSC` | AMD Ryzen 9 7940HS | Tested & Verified |
| **Minisforum UM780 XTX** | `F7BSD` | AMD Ryzen 7 7840HS | Supported |
| **Minisforum UM880 Pro** | `F7BSI` | AMD Ryzen 7 8845HS | Supported |
| **Minisforum UM890 Pro** | `F7BSW` | AMD Ryzen 9 8945HS | Supported |
| **Other UM-Series** | *Any* | AMD Phoenix / Hawk Point | Supported via Generic EC fallback |

---

## Installation

### Prerequisites (Arch Linux / CachyOS)

Ensure your kernel headers are installed:

```bash
sudo pacman -S linux-zen-headers base-devel dkms
```
*(Replace `linux-zen-headers` with `linux-headers` if using the standard kernel)*.

---

### Method 1: DKMS Installation (Recommended)

DKMS ensures the driver automatically rebuilds whenever your kernel updates:

```bash
# Clone the repository
git clone https://github.com/<your-username>/minisforum-platform.git
cd minisforum-platform

# Install via DKMS
sudo make dkms-install

# Load the module
sudo modprobe minisforum_platform
```

To enable auto-loading at boot:

```bash
echo "minisforum_platform" | sudo tee /etc/modules-load.d/minisforum_platform.conf
```

---

### Method 2: Manual Build & Load

```bash
# Build
make

# Test loading
sudo make load

# Check kernel logs
sudo dmesg | grep minisforum_platform

# Unload
sudo make unload
```

---

## Usage

### 1. ACPI Platform Profile (TDP Modes)

Once loaded, the driver registers with the Linux kernel's platform profile subsystem:

```bash
# View available choices
cat /sys/firmware/acpi/platform_profile_choices
# Output: low-power balanced performance

# View active profile
cat /sys/firmware/acpi/platform_profile

# Switch profile via sysfs
echo "performance" | sudo tee /sys/firmware/acpi/platform_profile
echo "balanced"    | sudo tee /sys/firmware/acpi/platform_profile
echo "low-power"   | sudo tee /sys/firmware/acpi/platform_profile
```

#### GNOME & KDE Integration
If you have `power-profiles-daemon` running:
```bash
# Using powerprofilesctl
powerprofilesctl set performance
powerprofilesctl get
```
The **Power Mode** slider will appear natively in the top-right Quick Settings menu of GNOME and KDE Plasma.

---

### 2. Hardware Monitoring (Sensors)

View temperature sensors and live fan speeds directly using `sensors`:

```bash
$ sensors minisforum-isa-0000
minisforum-isa-0000
Adapter: ISA adapter
CPU Fan:      969 RPM
System Fan:     0 RPM
CPU EC:       +42.0°C  
Motherboard:  +41.0°C  
VRM:          +39.0°C  
pwm1:             22%
```
*(Note: In default Auto mode, the hardware EC autonomous curve keeps the fan at a dead-silent ~950 RPM at idle, scaling up smoothly under load).*

---

### 3. Dynamic Thermal Governor & Fan Control

The driver includes an in-kernel autonomous thermal governor that eliminates the noisy, jumpy factory BIOS curves:

* **Composite Thermal Averaging:** Rather than reacting to a single instantaneous sensor reading, the governor computes a system-wide weighted average:
  $$T_{\text{composite}} = \frac{3 \times T_{\text{CPU}} + T_{\text{VRM}} + T_{\text{Motherboard}}}{5}$$
  This is filtered through an Exponential Moving Average (EMA) to prevent fan jitter from short 100ms CPU boost bursts.
* **Rock-Solid Silent Floor:** At idle ($< 46^\circ\text{C}$), the fan rests at **~550 - 600 RPM** (inaudible 0 dB).
* **Stepped Rate-Limiting & Hysteresis:**
  * **Heating up:** Fan steps up gradually (+1 target code / ~100 RPM per second).
  * **Cooling down:** Holds for a 4-second hysteresis cooldown before stepping down -1 code every 2 seconds.
* **AMD P-State & Scheduler Synergy:** Seamlessly coordinates with `amd-pstate=guided` (and `schedutil`) or `amd-pstate-epp` across `low-power`, `balanced`, and `performance` platform profiles.

#### Manual PWM Override
If you want to manually lock fan speeds, standard hwmon PWM controls remain available:

* **Mode Selection (`pwm1_enable`):**
  * `1` = **Manual Control** (locks fan to your desired speed)
  * `2` = **Auto Control** (restores in-kernel dynamic governor)

* **Speed Target (`pwm1`):**
  * Accepts standard Linux hwmon values from `0` to `255` (linearly mapped to EC target `0` to `5100 RPM`).

```bash
# Locate your minisforum hwmon path (e.g. hwmon9)
HWMON_DIR=$(dirname $(grep -l "minisforum" /sys/class/hwmon/hwmon*/name))

# Lock fan manually to ~1300 RPM (25% / code 13)
echo 65 | sudo tee $HWMON_DIR/pwm1

# Restore in-kernel dynamic governor (Auto Mode)
echo 2 | sudo tee $HWMON_DIR/pwm1_enable
```

> [!NOTE]
> **Hardware Safety Guard:** Even in manual mode, the hardware critical temperature row ($\ge 94^\circ\text{C}$) is never overridden. If the CPU temperature reaches $94^\circ\text{C}$, the EC firmware will automatically override manual settings to 100% full speed (5100 RPM) to protect the processor.

---

## Uninstallation

To remove the DKMS module:

```bash
sudo make dkms-uninstall
sudo rm -f /etc/modules-load.d/minisforum_platform.conf
```

---

## License

This project is licensed under the **GNU General Public License v2.0** (`GPL-2.0-only` or `GPL-2.0-or-later`).
See [LICENSE](LICENSE) for details.
