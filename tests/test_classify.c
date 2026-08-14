#include "../src/classify.h"

#include <stdio.h>
#include <stdlib.h>

struct test_case {
	const char *message;
	enum fault_event expected;
};

int main(void)
{
	static const struct test_case tests[] = {
		{"NVRM: Xid (PCI:0000:01:00): 119, Timeout after 5s", FAULT_NVIDIA_XID_119},
		{"NVRM: Xid (PCI:0000:01:00): 154, GPU recovery action changed", FAULT_NVIDIA_XID_154},
		{"NVRM: RPC sequence 119 is harmless here", FAULT_NONE},
		{"xhci_hcd 0000:00:14.0: HC died; cleaning up", FAULT_XHCI_DEAD},
		{"xhci_hcd 0000:00:0d.0: HC died; cleaning up", FAULT_NONE},
		{"DMAR: DRHD: handling fault status reg 2", FAULT_DMAR},
		{"DMAR: Intel(R) Virtualization Technology for Directed I/O", FAULT_NONE},
		{"xhci_hcd 0000:00:14.0: Event TRB for slot 7 ep 1 with no TDs queued", FAULT_XHCI_TRB},
		{"xhci_hcd 0000:00:14.0: TRB error for slot 9", FAULT_XHCI_TRB},
		{"usb 3-5: USB disconnect", FAULT_NONE},
	};

	for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
		enum fault_event actual = classify_message(tests[index].message);
		if (actual != tests[index].expected) {
			fprintf(stderr, "test %zu failed: expected %s, got %s\n", index,
			        fault_event_name(tests[index].expected), fault_event_name(actual));
			return EXIT_FAILURE;
		}
	}

	puts("classification tests passed");
	return EXIT_SUCCESS;
}
