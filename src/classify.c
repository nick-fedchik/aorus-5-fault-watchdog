#include "classify.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static long nvidia_xid(const char *message)
{
	const char *xid;
	const char *separator;
	char *end = NULL;
	long value;

	if (message == NULL || strstr(message, "NVRM:") == NULL) {
		return -1;
	}

	xid = strstr(message, "Xid (");
	if (xid == NULL) {
		return -1;
	}

	separator = strstr(xid, "): ");
	if (separator == NULL) {
		return -1;
	}
	separator += 3;
	while (isspace((unsigned char)*separator)) {
		separator++;
	}

	value = strtol(separator, &end, 10);
	if (end == separator) {
		return -1;
	}
	return value;
}

enum fault_event classify_message(const char *message)
{
	long xid;

	if (message == NULL) {
		return FAULT_NONE;
	}

	xid = nvidia_xid(message);
	if (xid == 79) {
		return FAULT_NVIDIA_XID_79;
	}
	if (xid == 119) {
		return FAULT_NVIDIA_XID_119;
	}
	if (xid == 154) {
		return FAULT_NVIDIA_XID_154;
	}

	if (strstr(message, "xhci_hcd 0000:00:14.0") != NULL &&
	    strstr(message, "HC died; cleaning up") != NULL) {
		return FAULT_XHCI_DEAD;
	}

	if (strstr(message, "DMAR:") != NULL &&
	    (strstr(message, "fault") != NULL ||
	     strstr(message, "FAULT") != NULL)) {
		return FAULT_DMAR;
	}

	if (strstr(message, "xhci_hcd 0000:00:14.0") != NULL &&
	    (strstr(message, "Event TRB") != NULL ||
	     strstr(message, "TRB error") != NULL ||
	     strstr(message, "Ring Underrun") != NULL ||
	     strstr(message, "no TDs queued") != NULL)) {
		return FAULT_XHCI_TRB;
	}

	return FAULT_NONE;
}

bool nvidia_probe_output_healthy(const char *output)
{
	if (output == NULL || output[0] == '\0') {
		return false;
	}
	if (strstr(output, "GPU-") == NULL) {
		return false;
	}
	if (strstr(output, "GPU requires reset") != NULL ||
	    strstr(output, "ERR!") != NULL) {
		return false;
	}
	return true;
}

const char *fault_event_name(enum fault_event event)
{
	switch (event) {
	case FAULT_NVIDIA_XID_79:
		return "nvidia-xid-79";
	case FAULT_NVIDIA_XID_119:
		return "nvidia-xid-119";
	case FAULT_NVIDIA_XID_154:
		return "nvidia-xid-154";
	case FAULT_NVIDIA_STARTUP_DEGRADED:
		return "nvidia-startup-degraded";
	case FAULT_XHCI_DEAD:
		return "xhci-hc-died";
	case FAULT_DMAR:
		return "dmar-fault";
	case FAULT_XHCI_TRB:
		return "xhci-trb";
	case FAULT_NONE:
	default:
		return "none";
	}
}
