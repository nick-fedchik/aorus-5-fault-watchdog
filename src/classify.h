#ifndef AORUS_FAULT_WATCHDOG_CLASSIFY_H
#define AORUS_FAULT_WATCHDOG_CLASSIFY_H

#include <stdbool.h>

enum fault_event {
	FAULT_NONE = 0,
	FAULT_NVIDIA_XID_79,
	FAULT_NVIDIA_XID_119,
	FAULT_NVIDIA_XID_154,
	FAULT_NVIDIA_STARTUP_DEGRADED,
	FAULT_XHCI_DEAD,
	FAULT_DMAR,
	FAULT_XHCI_TRB,
};

enum fault_event classify_message(const char *message);
const char *fault_event_name(enum fault_event event);
bool nvidia_probe_output_healthy(const char *output);

#endif
