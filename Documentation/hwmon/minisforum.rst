.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver minisforum_platform
=================================

Supported boards
----------------

The driver binds through DMI to Minisforum UM-series mini-PCs whose thermal
and power controls are implemented by an ITE IT5571 Embedded Controller
(EC). Currently matched boards are listed in ``mf_dmi_table`` in
``minisforum_core.c``; the DMI board name is reported in the kernel log when
the driver probes.

The EC registers used by this driver are not covered by a public datasheet.
They were determined from the firmware of the supported boards and are only
claimed for those boards.

Author: Dwi Mulia Mokoginta

Platform profile
----------------

The driver registers a ``platform_profile`` device, so the generic ACPI
platform profile interface is available::

  /sys/firmware/acpi/platform_profile
  /sys/firmware/acpi/platform_profile_choices

A profile selection is translated into the CPU power profile selector of the
EC. Power and thermal envelope arbitration stays entirely with the platform
firmware; the driver does not program CPU frequency, energy performance
preference, or power limits itself, and it exposes no module parameters.

Consumers such as ``tuned``, ``tuned-ppd`` or ``power-profiles-daemon`` are
expected to combine this interface with the cpufreq energy performance
preference where a user-visible change in behaviour is wanted. On the
``amd-pstate`` *active* mode there is no software governor to select, so EPP
is the meaningful knob for the CPU side.

When the EC changes the selector on its own -- the register is reset across
suspend, and firmware may override it -- the driver re-reads it on resume and
calls ``platform_profile_notify()`` so userspace is not left displaying a
stale value.

hwmon
-----

Temperatures
~~~~~~~~~~~~

======================  ==============================================
``temp1_input``         CPU temperature as reported by the EC
``temp2_input``         Motherboard temperature
``temp3_input``         VRM temperature
======================  ==============================================

Temperatures are exposed in millidegrees Celsius. ``temp1`` is the value the
EC uses for its own fan control; it is not the CPU die temperature reported
by ``k10temp``.

Fans
~~~~

``fan1`` is the CPU fan. ``fan2`` is the system fan header and is only visible
on boards that report more than one fan in their board configuration; it
reads 0 when no fan is attached.

PWM
~~~

The EC fan control is a lookup table of seven temperature bands. The driver
programs a fixed duty into every band, which makes the EC hold that duty by
itself; no periodic rewrite from the kernel is required. The hardware
protection row of the table (full speed from 94 degrees Celsius upwards) is
never written.

======================  ==============================================
``pwm1``                Fan duty, 0-255
``pwm1_enable``         Fan control mode, see below
``pwm1_mode``           Always 1 (PWM), read-only
======================  ==============================================

``pwm1_enable`` follows the standard hwmon semantics:

0
  No fan speed control, the fan runs at full speed.

1
  Manual. ``pwm1`` is owned by userspace, for example ``fancontrol`` or
  ``CoolerControl``. A custom curve belongs here, not in the kernel.

2
  Automatic (default). The fan is handed back to the EC firmware's own
  thermal curve, which the driver snapshots at probe.

Writing ``pwm1`` while not in mode 1 switches to manual mode, so tools that
write the duty before touching ``pwm1_enable`` also work.

Manual duty is clamped to a minimum of 25 so that a stray write of 0 cannot
stop the CPU fan. In mode 2 no single duty is programmed by the driver, so
``pwm1`` reads 0; use ``fan1_input`` for the resulting speed.

The Super I/O window used to reach the EC is claimed with
``request_muxed_region()``, because the generic ``it87`` driver claims the
same ports.

Unloading the module, or handing the fan back to the firmware, restores the
curve snapshot taken at probe, so the fan is never left at a userspace duty.
