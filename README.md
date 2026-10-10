# minisforum-platform

A modular Linux kernel platform driver for Minisforum Mini-PCs (UM790 Pro, UM780 XTX, UM880, and other Venus / EliteMini series).

This driver bridges missing OEM platform features on Linux through a clean, modular architecture:

* **TDP Switcher (`platform_profile`):** Native Linux ACPI platform profile integration (`low-power`, `balanced`, `performance`). Works out-of-the-box with GNOME Quick Settings, KDE Plasma, and `power-profiles-daemon`.
* **Dynamic Thermal Governor & Stepped EC Writes (Zero-Daemon):** Directly controls the embedded controller's (EC) hardware with a composite system thermal average (CPU die + VRM + Motherboard) filtered via Exponential Moving Average (EMA). Features a rock-solid inaudible ~550-600 RPM idle floor, gradual rate-limited step-up, and hysteresis step-down—ensuring zero erratic cycling, zero audible jumps, and native synergy with `amd-pstate=guided` / `schedutil`.
* **Hardware Monitoring & Manual Control (`hwmon`):** Exposes CPU, Motherboard, VRM temperature sensors, dual fan RPM tachometers, and manual PWM override controls (`pwm1` / `pwm1_enable`).
* **Multi-Model Support:** Built with a DMI quirk table that automatically recognizes and adapts to various Minisforum motherboard models.

---

## Hardware Compatibility Matrix

### Supported Models (Out of the Box)

| Device | Board Model | APU / Processor | Status | Fan / TDP Features |
| :--- | :--- | :--- | :--- | :--- |
| **Minisforum UM790 Pro** | `F7BSC` | AMD Ryzen 9 7940HS | Tested & Verified | Dynamic Gov, Tachometer, TDP Profile |
| **Minisforum UM780 XTX** | `F7BSD` | AMD Ryzen 7 7840HS | Supported | Dynamic Gov, Tachometer, TDP Profile |
| **Minisforum UM880 Pro** | `F7BSI` | AMD Ryzen 7 8845HS | Supported | Dynamic Gov, Tachometer, TDP Profile |
| **Minisforum UM890 Pro** | `F7BSW` | AMD Ryzen 9 8945HS | Supported | Dynamic Gov, Tachometer, TDP Profile |
| **Other UM-Series (AMD APU)** | *Any F7BSx* | AMD Phoenix / Hawk Point | Generic Fallback | Dynamic Gov, Tachometer, TDP Profile |

---

### Unsupported / Untested Models & Technical Reasons

Minisforum produces a diverse variety of Mini-PCs and motherboards across Intel, AMD dGPU, and desktop form factors. The table below details which series are currently **not supported** or **untested**, along with the underlying hardware architecture differences:

| Series / Model | Processor Architecture | Super I/O / EC Controller | Compatibility Status | Technical Reason |
| :--- | :--- | :--- | :--- | :--- |
| **Workstation Series**<br>(MS-01, MS-A1) | Intel Core 12th/13th Gen<br>or AMD AM5 Desktop | Nuvoton NCT6687D / ITE IT8613E | ❌ **Unsupported (Intel)**<br>⚠️ **Untested (MS-A1)** | Intel MS-01 uses desktop Nuvoton SIO and standard Intel DPTF/ACPI thermal zones, completely lacking the ITE IT5571 indirect SRAM layout. MS-A1 (AMD) may share EC similarities but requires testing. |
| **Intel Mini-PC Series**<br>(NPB5, NPB7, NAB6, NAB9, UN1265) | Intel Core i5/i7/i9 (Raptor/Alder Lake) | Nuvoton / ITE IT8613E | ❌ **Unsupported** | Different EC vendor and memory mapping. Fan curves and power limits are governed by Intel ACPI PECI/DPTF methods rather than laptop EC registers. |
| **Neptune dGPU Series**<br>(HX99G, HX100G, HX77G, G7 PT) | AMD Ryzen + Radeon RX 6600M / 6650M / 7600M XT | ITE IT8987 / Proprietary Dual EC | ⚠️ **Untested / Incompatible** | Features dual independent cooling systems (dedicated CPU blower + dedicated discrete GPU blower). Requires separate register offsets for GPU fan curve tables and dual tachometers. |
| **MoDT Mini-ITX Boards**<br>(BD770i, BD790i, AR900i) | Mobile-on-Desktop AMD Ryzen / Intel Core | Desktop Super I/O (NCT6796D / IT8686E) | ❌ **Not Applicable** | Standard desktop Mini-ITX motherboards with 4-pin desktop PWM fan headers. Supported natively in the upstream Linux kernel via `nct6687` or `it87` drivers. |
| **Ultra-Portable / Handhelds**<br>(EM680, EM780, V3 Tablet) | AMD Ryzen 7 6800U / 7840U / 8840U | ITE IT5570 / Custom USB-PD EC | ⚠️ **Untested (Experimental)** | Ultra-compact single-fan chassis governed by tight USB Type-C Power Delivery contracts. May share IT5570 registers, but thermal tables have different limits. |
| **Older AMD Venus Series**<br>(UM560, UM590, UM690, HM90) | AMD Ryzen 5000 / 6000 (Cezanne / Rembrandt) | ITE IT8528 / Legacy ITE EC | ⚠️ **Untested** | Older EC firmware generations that do not implement the `F7BSx` table format at ACPI `0x2F` / `0x0310`. |

---

## Technical Prerequisites (Compatibility Checklist)

For a Minisforum machine to be compatible with this driver, it must satisfy the following hardware requirements:

1. **Processor Platform:** AMD Ryzen Mobile / APU (Phoenix, Hawk Point, or newer Strix Point architectures).
2. **Embedded Controller (EC):**
   * **ITE IT5571E or IT5570E** Super I/O Embedded Controller.
   * Standard ACPI EC communication ports: Data port `0x62`, Command port `0x66`.
   * Super I/O indirect SRAM ports: Index port `0x2E`, Data port `0x2F` (accessed via ITE SMFI indirect addressing).
3. **Firmware Register Layout:**
   * **ACPI EC Address Space:**
     * `0x02` & `0x03`: EC Major & Minor Firmware Version.
     * `0x04`: VRM Temperature sensor (°C).
     * `0x05`: Motherboard Temperature sensor (°C).
     * `0x09`: CPU Die Temperature sensor (°C).
     * `0x2F`: CPU Platform Profile / TDP selector (`0x00` Quiet, `0xB1` Balanced, `0xB2` Performance).
   * **Super I/O SMFI XRAM Address Space:**
     * `0x0310 - 0x0322`: 7-band CPU fan base speed lookup table.
     * `0x08B0 - 0x08B6`: 7-band CPU fan slope lookup table.
     * `0x181E - 0x181F`: 16-bit CPU Fan tachometer counter (sampled at 2.156 MHz).

---

## Experimental Testing Guide for Other / New Minisforum Hardware

If you own an unlisted Minisforum Mini-PC (or a newly released model) and want to test compatibility or contribute hardware quirks, follow this step-by-step workflow:

### Step 1: Identify Your Motherboard Model & DMI

Check the hardware identification strings reported by your BIOS:

```bash
sudo dmidecode -s baseboard-product-name
# Examples: F7BSC (UM790 Pro), F7BSD (UM780 XTX), F7BSI (UM880 Pro)

sudo dmidecode -s system-product-name
# Example: UM790 Pro
```

### Step 2: Load the Driver Experimentally (`force_load=1`)

If your motherboard model is not yet in the official DMI quirk table, bypass the detection check using the `force_load` module parameter:

```bash
sudo modprobe minisforum_platform force_load=1
```

Check the kernel buffer to confirm initialization:

```bash
sudo dmesg | grep -i minisforum
```

You should see:
```text
minisforum_platform: FORCED LOAD requested via module parameter! Experimental mode active on unlisted hardware.
platform minisforum_platform: Platform profile registered successfully
platform minisforum_platform: HWMON sensors and Dynamic Thermal Governor registered
```

### Step 3: Verify Subsystem Responses

1. **Check Hardware Monitoring:**
   ```bash
   sensors minisforum-isa-0000
   ```
   * Do the CPU, Motherboard, and VRM temperatures report realistic non-zero values (e.g. 35°C – 70°C)?
   * Does the CPU Fan show a valid RPM reading (e.g. 500 – 2000 RPM)?

2. **Check Platform Profile Switching:**
   ```bash
   cat /sys/firmware/acpi/platform_profile_choices
   echo "performance" | sudo tee /sys/firmware/acpi/platform_profile
   cat /sys/firmware/acpi/platform_profile
   ```

### Step 4: Advanced EC Register Inspection & Dumping

To verify whether your machine uses the standard Minisforum EC memory layout, you can inspect the ACPI EC registers using the Linux kernel's `ec_sys` debug interface:

```bash
# Load ec_sys with write support (allows reading debugfs io)
sudo modprobe ec_sys write_support=1

# Read the first 64 bytes of EC RAM
sudo hexdump -C /sys/kernel/debug/ec/ec0/io | head -n 4
```

Look for:
* Offsets `0x02` and `0x03`: Should contain EC version bytes (e.g. `01 0c` for v1.12).
* Offsets `0x04`, `0x05`, `0x09`: Should contain fluctuating temperature bytes in hex (e.g., `0x28` = 40°C).
* Offset `0x2F`: Should reflect the profile code (`0x00`, `0xB1`, or `0xB2`).

### Step 5: Contributing Support for Your Model

If `force_load=1` works on your Minisforum machine, you can contribute permanent support:

1. Note the output of `sudo dmidecode -s baseboard-product-name`.
2. Add your board to `mf_dmi_table` in `src/minisforum_core.c`:
   ```c
   {
       .ident = "Minisforum YourModel",
       .matches = {
           DMI_MATCH(DMI_BOARD_NAME, "YOUR_BOARD_NAME"),
       },
       .driver_data = (void *)&board_um790_pro,
   },
   ```
3. Submit a Pull Request or open an Issue on GitHub with your `dmidecode` output and `sensors` log!

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

### 2. Autonomous CPU Governor & Frequency Coordination (`amd-pstate=guided` & active)

When running modern AMD processors with `amd-pstate=guided` or `amd-pstate=active`, standard Linux platform profile switches only adjust EC TDP without changing CPU frequency scaling governors. If left on generic `powersave`, the CPU gets pinned to its minimum base frequency (~1.11 GHz) with zero dynamic scaling, causing desktop micro-stutters.

`minisforum-platform` intelligently synchronizes cpufreq policies and CPPC EPP hints directly in-kernel via official Linux kernel interfaces (`freq_qos_*` and `cppc_set_epp`) with **zero userspace helpers / zero shell subprocesses**:

| ACPI Platform Profile | Asymmetric Per-Core CPPC EPP | In-Kernel Frequency QoS Range | Behavior & Architecture |
| :--- | :--- | :--- | :--- |
| **`low-power`** | **Gold Cores:** `balance_performance`<br>**Other Cores:** `power` (0xFF) | **1.75 GHz – 2.8 GHz** | Gold cores preserve 1.75 GHz floor to prevent UI lag; background cores enter deep power saving; whisper silent. |
| **`balanced`** | **Gold Cores:** `performance` (0x00)<br>**Other Cores:** `balance_performance` (0x80) | **2.2 GHz (Gold) / 1.75 GHz (Others) – 5.26 GHz Boost** | Asymmetric Preferred Cores: Interactive desktop loads steer to Gold Cores on `performance` for instant 5.26 GHz boost; remaining cores run on `balance_performance` with zero lag and whisper-silent fan (~600 RPM). |
| **`performance`** | **Primary Cores:** `performance` (0x00)<br>**SMT Threads:** `balance_performance` | **2.63 GHz (Primary) / 2.2 GHz (SMT) – 5.26 GHz Boost** | Blazing throughput; SMT threads avoid pumping excess voltage during non-saturated loads, keeping thermal headroom and fan acoustics optimal. |

#### Asymmetric Per-Core CPPC Architecture

The driver dynamically queries ACPI CPPC Preferred Cores (`cppc_get_highest_perf`) across all online CPUs to identify silicon silicon quality (Gold Cores, Silver Cores, Secondary Cores):
* **Gold Cores:** Highest silicon binning capable of highest frequencies at lowest voltages. Assigned aggressive EPP (`performance`) and responsive floors for instant desktop responsiveness.
* **Other Cores:** Assigned `balance_performance` on balanced mode to eliminate sluggishness while maintaining whisper-quiet fan acoustics.
* **Autonomous Boost Enforcement:** Driver guarantees `cpufreq_boost_set_sw()` remains enabled on `balanced` and `performance`.

#### Bidirectional Governor & Profile Synchronization

Synchronization is fully bidirectional:
* **Profile to Hardware:** Changing the ACPI platform profile (via GNOME Quick Settings, KDE Plasma, or `powerprofilesctl`) automatically applies the appropriate QoS frequency brackets, EPP hints, and EC TDP.
* **Policy to Profile:** If an external utility (e.g. `tlp`, `tuned`, GameMode, or a custom script) changes policy constraints:
  * Switching to `powersave` (capped) $\rightarrow$ automatically aligns the platform profile to **`low-power`** (EC Quiet 35W).
  * Switching to `performance` $\rightarrow$ automatically aligns the platform profile to **`performance`** (EC Turbo 65W).
  * Switching to `schedutil` / `balance_performance` $\rightarrow$ automatically aligns the platform profile to **`balanced`** (EC Balanced 54W).
  * Emits native `platform_profile_notify()` events so desktop UI sliders update in real time.

#### Non-AMD P-State Fallback

If the driver is loaded on hardware without active AMD P-State (e.g. an Intel machine or unsupported CPU architecture):
* Autonomous cpufreq QoS is automatically disabled (`manage_cpufreq = false`).
* Fan control safely preserves the factory BIOS autonomous curve, leaving hardware thermal management under default BIOS control.

#### Customizing Frequency Brackets at Runtime

You can customize frequency brackets per profile on the fly via sysfs:

```bash
# Customize Low-Power frequency brackets (e.g. 1.75 GHz - 2.8 GHz)
echo 1754353 | sudo tee /sys/devices/platform/minisforum_platform/freq_low_power_min
echo 2800000 | sudo tee /sys/devices/platform/minisforum_platform/freq_low_power_max

# Customize Balanced baseline floor
echo 1754353 | sudo tee /sys/devices/platform/minisforum_platform/freq_balanced_min

# Customize Performance baseline floor (e.g. 2.63 GHz)
echo 2631530 | sudo tee /sys/devices/platform/minisforum_platform/freq_performance_min

# Disable autonomous cpufreq management if you prefer manual cpupower control
echo 0 | sudo tee /sys/devices/platform/minisforum_platform/manage_cpufreq
```

#### Persistent Configuration via Modprobe

You can configure persistent defaults in `/etc/modprobe.d/minisforum_platform.conf`:

```ini
options minisforum_platform freq_low_power_min=1754353 freq_low_power_max=2800000 freq_balanced_min=1754353 freq_performance_min=2631530
```

---

### 3. Hardware Monitoring (Sensors)

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

### 4. Dynamic Thermal Governor & Fan Control

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
