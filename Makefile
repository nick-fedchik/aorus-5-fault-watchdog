CC ?= cc
PKG_CONFIG ?= pkg-config
VERSION ?= 0.2.0
PREFIX ?= /usr/local
DESTDIR ?=
BINDIR ?= $(PREFIX)/sbin
LIBEXECDIR ?= $(PREFIX)/libexec
DOCDIR ?= $(PREFIX)/share/doc/aorus-fault-watchdog
SYSTEMD_UNIT_DIR ?= /etc/systemd/system
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror \
	-DVERSION=\"$(VERSION)\" $(shell $(PKG_CONFIG) --cflags libsystemd)
LDLIBS += $(shell $(PKG_CONFIG) --libs libsystemd)

BUILD_DIR := build
TARGET := $(BUILD_DIR)/aorus-fault-watchdog
TEST_TARGET := $(BUILD_DIR)/test-classify

.PHONY: all clean install test uninstall

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(TARGET): src/main.c src/classify.c src/classify.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) src/main.c src/classify.c -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_TARGET): tests/test_classify.c src/classify.c src/classify.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_classify.c src/classify.c -o $@ $(LDFLAGS)

test: $(TEST_TARGET)
	$(TEST_TARGET)

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/aorus-fault-watchdog
	install -D -m 0755 scripts/capture.sh \
		$(DESTDIR)$(LIBEXECDIR)/aorus-fault-watchdog-capture
	install -D -m 0644 systemd/aorus-fault-watchdog.service \
		$(DESTDIR)$(SYSTEMD_UNIT_DIR)/aorus-fault-watchdog.service
	install -D -m 0644 README.md $(DESTDIR)$(DOCDIR)/README.md
	install -D -m 0644 LICENSE $(DESTDIR)$(DOCDIR)/LICENSE
	install -D -m 0644 AUTHORS.md $(DESTDIR)$(DOCDIR)/AUTHORS.md

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/aorus-fault-watchdog
	rm -f $(DESTDIR)$(LIBEXECDIR)/aorus-fault-watchdog-capture
	rm -f $(DESTDIR)$(SYSTEMD_UNIT_DIR)/aorus-fault-watchdog.service
	rm -f $(DESTDIR)$(DOCDIR)/README.md $(DESTDIR)$(DOCDIR)/LICENSE \
		$(DESTDIR)$(DOCDIR)/AUTHORS.md

clean:
	rm -rf $(BUILD_DIR)
