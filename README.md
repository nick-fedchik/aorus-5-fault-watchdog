# AORUS Fault Watchdog

An event-driven Linux service for capturing and recovering a specific fault
sequence observed on the **Gigabyte AORUS 5 SE4** laptop: NVIDIA GSP/runtime
power failures followed by failure of the Intel Alder Lake PCH xHCI controller.

## Author and research provenance

**Mykola Fedchyk** is the original author, maintainer and primary investigator
of the hardware/software fault sequence documented by this project.

The watchdog was developed from an extended cross-platform investigation on a
physical AORUS 5 SE4 system. The research correlated Linux kernel journals,
xHCI state, PCI runtime-power state, DMAR faults and NVIDIA GSP failures with
independent Windows USBXHCI LiveKernelEvent evidence from the same hardware.

The observations, incident chronology, diagnostic methodology and recovery
policy are original project research. They should be treated as evidence from
the tested machine, not as a claim that every AORUS 5 SE4 has the same defect.

For attribution and citation metadata, see [AUTHORS.md](AUTHORS.md) and
[CITATION.cff](CITATION.cff).

### AI-assisted development disclosure

The initial implementation, tests, systemd integration and documentation were
developed with assistance from **OpenAI Codex**. The local development session
recorded the following runtime information:

| Component | Recorded value |
|---|---|
| Interface | OpenAI Codex TUI (`codex-tui`) |
| Local CLI build identifier | `codex-cli 0.0.0` |
| Model | `gpt-5.6-sol` (GPT-5.6 Sol) |
| Reasoning effort | `medium` |
| Session date | 2026-08-14 |

The `0.0.0` value is the identifier reported by the locally installed Codex
build; it should not be interpreted as a normal public semantic release number.
GPT-5.6 Sol is documented in the
[official OpenAI model reference](https://developers.openai.com/api/docs/models/gpt-5.6-sol).

Mykola Fedchyk supplied the hardware, original cross-platform evidence,
research direction, safety constraints and live validation; reviewed the
findings and recovery behavior; and remains the author and maintainer. Codex is
disclosed as a development tool, not listed as a legal author or copyright
holder.

> [!WARNING]
> This project is experimental and intentionally hardware-specific. It writes
> to PCI driver sysfs interfaces and can temporarily disconnect every device on
> the affected USB controller. Read the recovery policy before installing it.

## Supported platform

The watchdog refuses to start unless all of these identifiers match:

| Component | Required value |
|---|---|
| System vendor | `GIGABYTE` |
| Product name reported by DMI | `AORUS 5 SE` |
| Marketed laptop model | AORUS 5 SE4 |
| Intel xHCI controller | `0000:00:14.0`, `8086:51ed` |
| NVIDIA GPU | `0000:01:00.0`, `10de:249d` |

Gigabyte firmware reports this SE4 SKU as `AORUS 5 SE` through DMI. Check a
machine without starting the daemon:

```bash
aorus-fault-watchdog --check-platform
```

The HP E27m/Genesys/RTL8153 USB-C topology motivated the original investigation,
but the watchdog is scoped to the laptop platform and PCI devices, not to that
monitor alone.

### Potential applicability to other AORUS 5 variants

The underlying defect may not be exclusive to the SE4. Other AORUS 5 variants
with a discrete NVIDIA GeForce RTX GPU may share relevant motherboard, ACPI,
embedded-controller, PCI power-management or Intel PCH xHCI design elements.
They could therefore be susceptible to the same general failure class:

```text
platform power event
  -> NVIDIA runtime-resume or GSP failure
  -> wider platform degradation
  -> Intel xHCI controller failure
```

This is a research hypothesis, not a confirmed compatibility claim. No other
AORUS 5 variant has yet been validated by this project, and differences in GPU,
BIOS, EC firmware, PCI IDs and USB topology may materially change the behavior.
The current daemon intentionally refuses to run automatic recovery on anything
except the tested SE4 identifiers. Reports from XE4, KE4 or other NVIDIA RTX
AORUS 5 systems would be valuable, but should begin with capture-only diagnosis
before any platform-specific recovery is enabled.

## Observed failure sequence

The incident that motivated the combined watchdog occurred in this order:

1. An ACPI power-source notification entered the NVIDIA runtime-resume path.
2. NVIDIA logged `Xid 119` GSP RPC timeouts.
3. NVIDIA logged `Xid 154`, requesting function-level recovery.
4. About two minutes later, `0000:00:14.0` logged
   `HC died; cleaning up` and all devices on USB Bus 003 disappeared.
5. An xHCI unbind/bind cycle restored the USB keyboard, mouse and other devices
   without rebooting. The GPU still required a reboot.

Temporal correlation does not prove that NVIDIA directly causes xHCI failure;
both failures may result from a shared platform power or firmware defect.

## Technology

The core daemon is written in C17 and links dynamically to `libsystemd`:

- `sd-journal` follows new kernel-journal records without spawning or polling
  `journalctl`;
- `sd_notify` provides readiness and heartbeat notifications;
- systemd supervises the daemon with `WatchdogSec=45s`;
- event classification is isolated in a small, unit-tested module;
- a root-only Bash helper collects diagnostic state only after an event.

The daemon never polls `nvidia-smi`. That command can block or return partial
data after a GSP failure.

## Events and actions

| Event | Diagnostic capture | Automatic action |
|---|---:|---|
| NVIDIA `Xid 119` | Yes | Set GPU `power/control=on` |
| NVIDIA `Xid 154` | Rate-limited | Set GPU `power/control=on` |
| DMAR fault | Yes | None |
| xHCI TRB/ring corruption | Yes | None |
| Exact `00:14.0: HC died; cleaning up` | Always, before recovery | xHCI unbind/bind |

Safety boundaries:

- the GPU is never reset automatically;
- the machine is never rebooted automatically;
- xHCI recovery occurs only after the exact `HC died` message;
- xHCI rebind is limited to once per 10 minutes;
- general captures are limited to once per 30 seconds;
- an xHCI death within 180 seconds of an NVIDIA Xid is marked as correlated;
- only new journal entries after daemon startup are processed.

## Diagnostic output

systemd creates the state directory automatically. Incidents are stored under:

```text
/var/lib/aorus-fault-watchdog/incidents/
```

Each incident contains an event record and a timestamped capture with relevant
kernel journal entries, USB topology, PCI state, runtime-power state, input
devices, interrupts and available xHCI debugfs metadata.

These files may contain hostnames, hardware identifiers and device serial
numbers. Review them before attaching them to a public GitHub issue.

## Requirements

- Linux with systemd and a persistent kernel journal
- GCC or Clang with C17 support
- GNU Make
- `pkg-config`
- `libsystemd` development headers
- `usbutils` and `pciutils` for diagnostic captures

On Ubuntu:

```bash
sudo apt install build-essential pkg-config libsystemd-dev usbutils pciutils
```

## Validated development and test environment

The initial release was built and tested on the physical system where the fault
was investigated:

| Component | Tested value |
|---|---|
| Laptop | Gigabyte AORUS 5 SE4 |
| Firmware DMI product name | `AORUS 5 SE` |
| Operating system | Ubuntu 26.04 LTS (`Resolute Raccoon`) |
| Kernel | `7.0.0-29-generic` |
| Architecture | `x86_64` |
| CPU | Intel Core i7-12700H |
| NVIDIA GPU | GeForce RTX 3070 Laptop GPU, `10de:249d` |
| NVIDIA driver | `610.57.04` |
| Intel xHCI | Alder Lake PCH USB 3.2, `8086:51ed`, revision `01` |
| BIOS | `FB0F`, dated 2026-03-23 |
| USB-C monitor/hub | HP E27m G4 with Genesys hubs and RTL8153 Ethernet |
| C compiler | GCC `15.2.0` |
| GNU Make | `4.4.1` |
| libsystemd | `259` |
| glibc | `2.43` |

GitHub Actions provides an additional clean build and classifier-test check,
but automatic hardware recovery can only be validated on matching physical
hardware.

## Build and test

```bash
make
make test
```

The build enables `-Wall -Wextra -Wpedantic -Werror`. The tests exercise known
NVIDIA, DMAR and xHCI messages as well as negative cases.

Classifier-only checks do not require root and perform no recovery action:

```bash
build/aorus-fault-watchdog --classify \
  'xhci_hcd 0000:00:14.0: HC died; cleaning up'
```

## Install

Review the unit and source first, then run:

```bash
make
make test
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now aorus-fault-watchdog.service
```

Verify the installation:

```bash
/usr/local/sbin/aorus-fault-watchdog --version
/usr/local/sbin/aorus-fault-watchdog --check-platform
systemctl status aorus-fault-watchdog.service
journalctl -u aorus-fault-watchdog.service -f
```

## Installed files

```text
/usr/local/sbin/aorus-fault-watchdog
/usr/local/libexec/aorus-fault-watchdog-capture
/usr/local/share/doc/aorus-fault-watchdog/README.md
/usr/local/share/doc/aorus-fault-watchdog/LICENSE
/usr/local/share/doc/aorus-fault-watchdog/AUTHORS.md
/etc/systemd/system/aorus-fault-watchdog.service
/var/lib/aorus-fault-watchdog/incidents/
```

## Uninstall

Stop and disable the service before removing its files:

```bash
sudo systemctl disable --now aorus-fault-watchdog.service
sudo make uninstall
sudo systemctl daemon-reload
```

`make uninstall` intentionally leaves `/var/lib/aorus-fault-watchdog` intact so
that diagnostic evidence is not deleted accidentally.

## Limitations

- This service mitigates and records failures; it does not repair the underlying
  BIOS, EC, xHCI silicon, NVIDIA firmware or driver defect.
- A recovered xHCI bus does not imply that a GPU affected by `Xid 154` is healthy.
- A complete hardware lock may stop the journal before its final messages reach
  disk. Remote netconsole or a functioning hardware watchdog is needed for that
  failure class.
- The tested laptop currently exposes no `/dev/watchdog` device. The systemd
  watchdog supervises this daemon only; it is not a machine-level watchdog.
- PCI addresses and IDs are intentionally fixed for the supported AORUS 5 SE4.

## Development

Project layout:

```text
src/        C daemon and event classifier
tests/      classifier unit tests
scripts/    privileged diagnostic capture helper
systemd/    service unit
```

Keep automatic actions conservative. New event patterns should first be added
as capture-only events with positive and negative classifier tests.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
