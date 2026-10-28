# Minisforum driver — submission notes

**Berkas ini bukan bagian dari patch series.** Isinya hunk yang harus
diterapkan ke file milik tree kernel, yang tidak ada di repo ini.

## 1. Hook di `drivers/platform/x86/Kconfig`

Tambahkan satu baris `source` (urut abjad di antara entri yang ada):

```diff
+source "drivers/platform/x86/minisforum/Kconfig"
```

## 2. Hook di `drivers/platform/x86/Makefile`

```diff
+obj-y += minisforum/
```

## 3. `Documentation/hwmon/index.rst`

```diff
+   minisforum
```

## 4. `MAINTAINERS`

Sisipkan blok berikut pada urutan abjad (`MINISFORUM...`):

```
MINISFORUM UM SERIES PLATFORM AND HWMON DRIVER
M:	Dwi Mulia Mokoginta <dwi-mulia-mokoginta@protonmail.com>
L:	platform-driver-x86@vger.kernel.org
L:	linux-hwmon@vger.kernel.org
S:	Maintained
W:	https://github.com/malwareslayer/minisforum-platform
F:	Documentation/hwmon/minisforum.rst
F:	drivers/platform/x86/minisforum/
```

## 5. Urutan patch series

1. `platform/x86/minisforum: add EC core and DMI matching`
2. `platform/x86/minisforum: add ACPI platform profile support`
3. `platform/x86/minisforum: add hwmon temperature and fan support`
4. `hwmon: add minisforum driver documentation`

Tiap patch harus bisa dikompilasi sendiri. Patch 1 sengaja belum memanggil
`minisforum_profile_init()` / `minisforum_hwmon_init()`; keduanya masuk pada
patch 2 dan 3.

## 6. Draf cover letter

```
Subject: [PATCH 0/4] platform/x86: add Minisforum UM series driver

This series adds a driver for the Minisforum UM-series mini-PCs, whose
thermal and power controls are implemented by an ITE IT5571 Embedded
Controller.

The driver is deliberately mechanism-only:

  * the ACPI platform profile selector is translated into the EC's CPU
    power profile register. Power and thermal envelope arbitration stays
    with the platform firmware. No CPU frequency, energy performance
    preference, power limit or NVMe power management is programmed by the
    driver, and there are no module parameters.

  * fan control is exposed through the standard hwmon ABI. pwm1_enable=2
    hands the fan back to the EC firmware's own curve, which is snapshotted
    at probe; pwm1_enable=1 gives userspace ownership for fancontrol,
    CoolerControl and similar. No thermal policy lives in the kernel.

Two points I would like review input on:

  * pwm1 reads 0 in automatic mode because no single duty is programmed by
    the driver. Reporting a synthetic value seemed worse than reporting
    nothing, but I am open to suggestions.

  * writing pwm1 while not in mode 1 switches to manual mode instead of
    returning -EPERM, so tools that write the duty before pwm1_enable work.

The EC register layout used here is not covered by a public datasheet; it
was determined from the firmware of the listed boards, which is why binding
is restricted to those DMI names.

Patch 1 also claims the Super I/O window with request_muxed_region(), since
the generic it87 driver claims the same ports.
```

## 7. Pemeriksaan sebelum kirim

```bash
# gaya
perl scripts/checkpatch.pl --strict --no-tree -f \
    drivers/platform/x86/minisforum/*.c \
    drivers/platform/x86/minisforum/minisforum.h

# penerima
perl scripts/get_maintainer.pl --no-git -f \
    drivers/platform/x86/minisforum/*

# kompilasi in-tree (belum pernah diuji di sini; tree kernel lokal parsial)
make M=drivers/platform/x86/minisforum CONFIG_MINISFORUM_PLATFORM=m modules

# patch
git format-patch --cover-letter -4
```
