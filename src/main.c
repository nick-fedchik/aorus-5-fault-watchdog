#define _GNU_SOURCE

#include "classify.h"

#include <systemd/sd-daemon.h>
#include <systemd/sd-journal.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_STATE_DIRECTORY "/var/lib/aorus-fault-watchdog"
#define CAPTURE_SCRIPT "/usr/local/libexec/aorus-fault-watchdog-capture"
#define WORKAROUND_SCRIPT "/usr/local/sbin/aorus-idle-workaround"
#define NVIDIA_POWER_CONTROL "/sys/bus/pci/devices/0000:01:00.0/power/control"
#define NVIDIA_AUDIO_POWER_CONTROL "/sys/bus/pci/devices/0000:01:00.1/power/control"
#define NVIDIA_SMI "/usr/bin/nvidia-smi"
#define STARTUP_PROBE_OUTPUT "/run/aorus-fault-watchdog/startup-nvidia-smi.txt"
#define BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#define XHCI_UNBIND "/sys/bus/pci/drivers/xhci_hcd/unbind"
#define XHCI_BIND "/sys/bus/pci/drivers/xhci_hcd/bind"
#define LOCK_FILE "/run/aorus-fault-watchdog.lock"

#define CAPTURE_RATE_LIMIT_SEC 30
#define REBIND_RATE_LIMIT_SEC 600
#define CORRELATION_WINDOW_SEC 180
#define CAPTURE_TIMEOUT_SEC 30
#define NVIDIA_PROBE_TIMEOUT_SEC 10
#define EXIT_UNSUPPORTED_PLATFORM 78

#define DMI_SYS_VENDOR "/sys/class/dmi/id/sys_vendor"
#define DMI_PRODUCT_NAME "/sys/class/dmi/id/product_name"
#define XHCI_VENDOR_ID "/sys/bus/pci/devices/0000:00:14.0/vendor"
#define XHCI_DEVICE_ID "/sys/bus/pci/devices/0000:00:14.0/device"
#define NVIDIA_VENDOR_ID "/sys/bus/pci/devices/0000:01:00.0/vendor"
#define NVIDIA_DEVICE_ID "/sys/bus/pci/devices/0000:01:00.0/device"

static volatile sig_atomic_t stop_requested;

static int read_trimmed(const char *path, char *buffer, size_t buffer_size)
{
	int fd;
	ssize_t length;

	if (buffer_size < 2) {
		return -EINVAL;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -errno;
	}
	length = read(fd, buffer, buffer_size - 1);
	if (length < 0) {
		int saved = errno;
		close(fd);
		return -saved;
	}
	close(fd);
	buffer[length] = '\0';
	while (length > 0 &&
	       (buffer[length - 1] == '\n' || buffer[length - 1] == '\r' ||
	        buffer[length - 1] == ' ' || buffer[length - 1] == '\t')) {
		buffer[--length] = '\0';
	}
	return 0;
}

static int check_value(const char *path, const char *expected, const char *label,
	                   char *reason, size_t reason_size)
{
	char actual[128];
	int result = read_trimmed(path, actual, sizeof(actual));

	if (result < 0) {
		snprintf(reason, reason_size, "cannot read %s: %s", label, strerror(-result));
		return result;
	}
	if (strcmp(actual, expected) != 0) {
		snprintf(reason, reason_size, "%s mismatch: expected '%s', found '%s'",
		         label, expected, actual);
		return -ENODEV;
	}
	return 0;
}

static int platform_supported(char *reason, size_t reason_size)
{
	struct platform_value {
		const char *path;
		const char *expected;
		const char *label;
	};
	static const struct platform_value values[] = {
		{DMI_SYS_VENDOR, "GIGABYTE", "DMI system vendor"},
		{DMI_PRODUCT_NAME, "AORUS 5 SE", "DMI product name"},
		{XHCI_VENDOR_ID, "0x8086", "xHCI vendor ID"},
		{XHCI_DEVICE_ID, "0x51ed", "xHCI device ID"},
		{NVIDIA_VENDOR_ID, "0x10de", "NVIDIA vendor ID"},
		{NVIDIA_DEVICE_ID, "0x249d", "NVIDIA device ID"},
	};

	for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
		int result = check_value(values[index].path, values[index].expected,
		                         values[index].label, reason, reason_size);
		if (result < 0) {
			return result;
		}
	}
	snprintf(reason, reason_size,
	         "supported Gigabyte AORUS 5 SE4 platform (DMI: AORUS 5 SE)");
	return 0;
}

static void on_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static int64_t monotonic_seconds(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
		return 0;
	}
	return (int64_t)ts.tv_sec;
}

static void log_message(int priority, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	sd_journal_printv(priority, format, args);
	va_end(args);
}

static int write_value(const char *path, const char *value)
{
	int fd;
	ssize_t length = (ssize_t)strlen(value);
	ssize_t written;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		return -errno;
	}
	written = write(fd, value, (size_t)length);
	if (written != length) {
		int saved = written < 0 ? errno : EIO;
		close(fd);
		return -saved;
	}
	if (close(fd) < 0) {
		return -errno;
	}
	return 0;
}

static int mkdir_if_needed(const char *path, mode_t mode)
{
	if (mkdir(path, mode) == 0 || errno == EEXIST) {
		return 0;
	}
	return -errno;
}

static int claim_startup_degraded_capture(void)
{
	const char *state_directory = getenv("STATE_DIRECTORY");
	char boot_id[64];
	char marker_path[1024];
	int fd;
	int result;

	if (state_directory == NULL || state_directory[0] != '/') {
		state_directory = DEFAULT_STATE_DIRECTORY;
	}
	result = read_trimmed(BOOT_ID_PATH, boot_id, sizeof(boot_id));
	if (result < 0) {
		return result;
	}
	result = mkdir_if_needed(state_directory, 0750);
	if (result < 0) {
		return result;
	}
	result = snprintf(marker_path, sizeof(marker_path),
	                  "%s/startup-degraded-%s.marker", state_directory, boot_id);
	if (result < 0 || (size_t)result >= sizeof(marker_path)) {
		return -ENAMETOOLONG;
	}

	fd = open(marker_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	if (fd < 0) {
		return errno == EEXIST ? 0 : -errno;
	}
	if (close(fd) < 0) {
		return -errno;
	}
	return 1;
}

static int create_incident_dir(enum fault_event event, char *out, size_t out_size)
{
	const char *state_directory = getenv("STATE_DIRECTORY");
	char incident_root[1024];
	time_t now = time(NULL);
	struct tm local;
	char timestamp[32];
	int result;

	if (localtime_r(&now, &local) == NULL ||
	    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H-%M-%S%z", &local) == 0) {
		return -EINVAL;
	}

	if (state_directory == NULL || state_directory[0] != '/') {
		state_directory = DEFAULT_STATE_DIRECTORY;
	}
	result = snprintf(incident_root, sizeof(incident_root), "%s/incidents",
	                  state_directory);
	if (result < 0 || (size_t)result >= sizeof(incident_root)) {
		return -ENAMETOOLONG;
	}
	result = mkdir_if_needed(state_directory, 0750);
	if (result < 0) {
		return result;
	}
	result = mkdir_if_needed(incident_root, 0750);
	if (result < 0) {
		return result;
	}

	result = snprintf(out, out_size, "%s/%s-%s-%ld", incident_root,
	                  timestamp, fault_event_name(event), (long)getpid());
	if (result < 0 || (size_t)result >= out_size) {
		return -ENAMETOOLONG;
	}
	return mkdir_if_needed(out, 0755);
}

static int write_event_metadata(const char *directory, enum fault_event event,
	                            const char *message, bool correlated)
{
	char path[1024];
	char timestamp[64];
	char buffer[8192];
	time_t now = time(NULL);
	struct tm local;
	int fd;
	int length;

	if (localtime_r(&now, &local) == NULL ||
	    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &local) == 0) {
		return -EINVAL;
	}
	length = snprintf(path, sizeof(path), "%s/event.txt", directory);
	if (length < 0 || (size_t)length >= sizeof(path)) {
		return -ENAMETOOLONG;
	}
	length = snprintf(buffer, sizeof(buffer),
	                  "timestamp=%s\nevent=%s\ncorrelated_nvidia_to_xhci=%s\nmessage=%s\n",
	                  timestamp, fault_event_name(event), correlated ? "yes" : "no",
	                  message != NULL ? message : "");
	if (length < 0 || (size_t)length >= sizeof(buffer)) {
		return -EOVERFLOW;
	}

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0) {
		return -errno;
	}
	if (write(fd, buffer, (size_t)length) != length) {
		int saved = errno != 0 ? errno : EIO;
		close(fd);
		return -saved;
	}
	if (close(fd) < 0) {
		return -errno;
	}
	return 0;
}

static int wait_child_with_timeout(pid_t pid, unsigned int timeout_seconds)
{
	int status;
	unsigned int ticks = timeout_seconds * 10;
	struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};

	for (unsigned int tick = 0; tick < ticks; tick++) {
		pid_t result = waitpid(pid, &status, WNOHANG);
		if (result == pid) {
			if (WIFEXITED(status)) {
				return WEXITSTATUS(status) == 0 ? 0 : -EIO;
			}
			return -EINTR;
		}
		if (result < 0) {
			return -errno;
		}
		nanosleep(&delay, NULL);
	}

	kill(-pid, SIGTERM);
	nanosleep(&delay, NULL);
	kill(-pid, SIGKILL);
	waitpid(pid, &status, 0);
	return -ETIMEDOUT;
}

static int run_program(const char *program, char *const argv[], unsigned int timeout_seconds,
	                   const char *output_path)
{
	pid_t pid = fork();

	if (pid < 0) {
		return -errno;
	}
	if (pid == 0) {
		int fd;

		setpgid(0, 0);
		if (output_path != NULL) {
			fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0) {
				_exit(126);
			}
			if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
				_exit(126);
			}
			close(fd);
		}
		execv(program, argv);
		_exit(127);
	}
	setpgid(pid, pid);
	return wait_child_with_timeout(pid, timeout_seconds);
}

static int run_capture_sync(const char *incident_dir)
{
	char *const argv[] = {(char *)CAPTURE_SCRIPT, (char *)incident_dir, NULL};
	return run_program(CAPTURE_SCRIPT, argv, CAPTURE_TIMEOUT_SEC, NULL);
}

static void run_capture_async(const char *incident_dir)
{
	pid_t pid = fork();

	if (pid < 0) {
		log_message(LOG_ERR, "cannot fork capture worker: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		char *const argv[] = {(char *)CAPTURE_SCRIPT, (char *)incident_dir, NULL};
		setpgid(0, 0);
		execv(CAPTURE_SCRIPT, argv);
		_exit(127);
	}
	log_message(LOG_INFO, "capture worker pid=%ld directory=%s", (long)pid, incident_dir);
}

static int force_nvidia_power_on(bool after_fault)
{
	int gpu_result = write_value(NVIDIA_POWER_CONTROL, "on");
	int audio_result = write_value(NVIDIA_AUDIO_POWER_CONTROL, "on");

	if (gpu_result < 0) {
		log_message(LOG_ERR, "cannot force NVIDIA GPU function power on: %s",
		            strerror(-gpu_result));
	}
	if (audio_result < 0 && audio_result != -ENOENT) {
		log_message(LOG_ERR, "cannot force NVIDIA audio function power on: %s",
		            strerror(-audio_result));
	}
	if (gpu_result == 0 && (audio_result == 0 || audio_result == -ENOENT)) {
		log_message(after_fault ? LOG_WARNING : LOG_INFO,
		            after_fault
		                ? "NVIDIA PCI function runtime power forced on after fault event"
		                : "NVIDIA PCI function runtime power forced on at startup");
	}
	if (gpu_result < 0) {
		return gpu_result;
	}
	if (audio_result < 0 && audio_result != -ENOENT) {
		return audio_result;
	} else {
		return 0;
	}
}

static bool is_nvidia_event(enum fault_event event)
{
	return event == FAULT_NVIDIA_XID_79 || event == FAULT_NVIDIA_XID_119 ||
	       event == FAULT_NVIDIA_XID_154 ||
	       event == FAULT_NVIDIA_STARTUP_DEGRADED;
}

static int recover_xhci(const char *incident_dir)
{
	char output_path[1024];
	int result;
	char *const workaround_argv[] = {(char *)WORKAROUND_SCRIPT, "apply", NULL};
	char *const lsusb_argv[] = {"/usr/bin/lsusb", "-t", NULL};

	result = write_value(XHCI_UNBIND, "0000:00:14.0");
	if (result < 0) {
		log_message(LOG_ERR, "xHCI unbind failed: %s", strerror(-result));
		return result;
	}
	sleep(1);
	result = write_value(XHCI_BIND, "0000:00:14.0");
	if (result < 0) {
		log_message(LOG_ERR, "xHCI bind failed: %s", strerror(-result));
		return result;
	}

	(void)run_program(WORKAROUND_SCRIPT, workaround_argv, 15, NULL);
	sleep(3);
	if (snprintf(output_path, sizeof(output_path), "%s/post-recovery-lsusb-t.txt",
	             incident_dir) < (int)sizeof(output_path)) {
		(void)run_program("/usr/bin/lsusb", lsusb_argv, 10, output_path);
	}
	log_message(LOG_WARNING, "xHCI controller rebound after HC died; directory=%s",
	            incident_dir);
	return 0;
}

static int handle_event(enum fault_event event, const char *message,
	                    int64_t *last_capture, int64_t *last_rebind,
	                    int64_t *last_nvidia_fault)
{
	int64_t now = monotonic_seconds();
	bool nvidia_event = is_nvidia_event(event);
	bool correlated = false;
	bool should_capture;
	char incident_dir[1024];
	int result;

	if (nvidia_event) {
		*last_nvidia_fault = now;
		(void)force_nvidia_power_on(true);
	}
	if (event == FAULT_XHCI_DEAD && *last_nvidia_fault > 0 &&
	    now - *last_nvidia_fault <= CORRELATION_WINDOW_SEC) {
		correlated = true;
		log_message(LOG_CRIT,
		            "correlated incident: NVIDIA fault preceded xHCI death by %lld seconds",
		            (long long)(now - *last_nvidia_fault));
	}

	log_message(event == FAULT_XHCI_DEAD ? LOG_CRIT : LOG_WARNING,
	            "detected event=%s message=%s", fault_event_name(event), message);

	should_capture = *last_capture == 0 || now - *last_capture >= CAPTURE_RATE_LIMIT_SEC ||
	                 event == FAULT_XHCI_DEAD;
	if (!should_capture) {
		log_message(LOG_INFO, "capture rate-limited for event=%s", fault_event_name(event));
		return 0;
	}

	result = create_incident_dir(event, incident_dir, sizeof(incident_dir));
	if (result < 0) {
		log_message(LOG_ERR, "cannot create incident directory: %s", strerror(-result));
		return result;
	}
	*last_capture = now;
	(void)write_event_metadata(incident_dir, event, message, correlated);

	if (event != FAULT_XHCI_DEAD) {
		run_capture_async(incident_dir);
		return 0;
	}

	result = run_capture_sync(incident_dir);
	if (result < 0) {
		log_message(LOG_ERR, "pre-rebind capture failed: %s", strerror(-result));
	}
	if (*last_rebind > 0 && now - *last_rebind < REBIND_RATE_LIMIT_SEC) {
		log_message(LOG_CRIT, "xHCI rebind suppressed by %d-second rate limit",
		            REBIND_RATE_LIMIT_SEC);
		return 0;
	}
	*last_rebind = now;
	return recover_xhci(incident_dir);
}

static void check_nvidia_startup_health(int64_t *last_capture,
	                                   int64_t *last_rebind,
	                                   int64_t *last_nvidia_fault)
{
	char message[256];
	char probe_output[1024];
	char *const argv[] = {(char *)NVIDIA_SMI,
	                      "--query-gpu=uuid,pstate,temperature.gpu",
	                      "--format=csv,noheader", NULL};
	int result;
	int capture_claim;

	if (access(NVIDIA_SMI, X_OK) < 0) {
		log_message(LOG_WARNING, "startup GPU health probe skipped: %s is unavailable",
		            NVIDIA_SMI);
		return;
	}

	result = run_program(NVIDIA_SMI, argv, NVIDIA_PROBE_TIMEOUT_SEC,
	                     STARTUP_PROBE_OUTPUT);
	if (result == 0) {
		result = read_trimmed(STARTUP_PROBE_OUTPUT, probe_output,
		                      sizeof(probe_output));
	}
	if (result == 0 && nvidia_probe_output_healthy(probe_output)) {
		log_message(LOG_INFO, "startup GPU health probe passed");
		return;
	}

	if (result == -ETIMEDOUT) {
		snprintf(message, sizeof(message),
		         "startup nvidia-smi health probe timed out after %d seconds",
		         NVIDIA_PROBE_TIMEOUT_SEC);
	} else if (result < 0) {
		snprintf(message, sizeof(message),
		         "startup nvidia-smi health probe failed: %s", strerror(-result));
	} else {
		snprintf(message, sizeof(message),
		         "startup nvidia-smi health probe reported degraded GPU state");
	}
	capture_claim = claim_startup_degraded_capture();
	if (capture_claim == 0) {
		log_message(LOG_WARNING,
		            "%s; incident capture already completed during this boot", message);
		return;
	}
	if (capture_claim < 0) {
		log_message(LOG_ERR, "cannot create startup-degraded boot marker: %s",
		            strerror(-capture_claim));
	}
	(void)handle_event(FAULT_NVIDIA_STARTUP_DEGRADED, message, last_capture,
	                   last_rebind, last_nvidia_fault);
}

static int monitor_journal(void)
{
	sd_journal *journal = NULL;
	int64_t last_capture = 0;
	int64_t last_rebind = 0;
	int64_t last_nvidia_fault = 0;
	int result;

	result = sd_journal_open(&journal, SD_JOURNAL_LOCAL_ONLY);
	if (result < 0) {
		log_message(LOG_ERR, "cannot open journal: %s", strerror(-result));
		return result;
	}
	result = sd_journal_add_match(journal, "_TRANSPORT=kernel", 0);
	if (result < 0) {
		log_message(LOG_ERR, "cannot add journal match: %s", strerror(-result));
		sd_journal_close(journal);
		return result;
	}
	result = sd_journal_seek_tail(journal);
	if (result < 0) {
		log_message(LOG_ERR, "cannot seek journal tail: %s", strerror(-result));
		sd_journal_close(journal);
		return result;
	}

	(void)force_nvidia_power_on(false);
	check_nvidia_startup_health(&last_capture, &last_rebind, &last_nvidia_fault);

	sd_notify(0, "READY=1\nSTATUS=Monitoring NVIDIA/xHCI/DMAR kernel events");
	log_message(LOG_INFO,
	            "watchdog ready; startup GPU probe complete, monitoring new kernel events");

	while (!stop_requested) {
		while ((result = sd_journal_next(journal)) > 0) {
			const void *raw = NULL;
			size_t length = 0;
			char *field;
			const char *message;
			enum fault_event event;

			if (sd_journal_get_data(journal, "MESSAGE", &raw, &length) < 0 || length <= 8) {
				continue;
			}
			field = strndup(raw, length);
			if (field == NULL) {
				continue;
			}
			message = field + 8;
			event = classify_message(message);
			if (event != FAULT_NONE) {
				(void)handle_event(event, message, &last_capture, &last_rebind,
				                   &last_nvidia_fault);
			}
			free(field);
			while (waitpid(-1, NULL, WNOHANG) > 0) {
			}
		}
		if (result < 0) {
			log_message(LOG_ERR, "journal iteration failed: %s", strerror(-result));
			break;
		}
		sd_notify(0, "WATCHDOG=1");
		result = sd_journal_wait(journal, 10 * 1000 * 1000);
		while (waitpid(-1, NULL, WNOHANG) > 0) {
		}
		if (result < 0 && result != -EINTR) {
			log_message(LOG_ERR, "journal wait failed: %s", strerror(-result));
			break;
		}
	}

	sd_notify(0, "STOPPING=1");
	sd_journal_close(journal);
	return result < 0 ? result : 0;
}

static int acquire_lock(void)
{
	int fd = open(LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);

	if (fd < 0) {
		return -errno;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		int saved = errno;
		close(fd);
		return -saved;
	}
	return fd;
}

int main(int argc, char **argv)
{
	struct sigaction action = {.sa_handler = on_signal};
	char platform_reason[256];
	int lock_fd;
	int result;

	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("aorus-fault-watchdog %s\n", VERSION);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--check-platform") == 0) {
		result = platform_supported(platform_reason, sizeof(platform_reason));
		printf("%s\n", platform_reason);
		return result < 0 ? EXIT_UNSUPPORTED_PLATFORM : 0;
	}
	if (argc == 3 && strcmp(argv[1], "--classify") == 0) {
		puts(fault_event_name(classify_message(argv[2])));
		return 0;
	}
	if (argc != 1) {
		fprintf(stderr,
		        "Usage: %s [--version|--check-platform|--classify MESSAGE]\n",
		        argv[0]);
		return 2;
	}
	result = platform_supported(platform_reason, sizeof(platform_reason));
	if (result < 0) {
		log_message(LOG_ERR, "unsupported platform: %s", platform_reason);
		return EXIT_UNSUPPORTED_PLATFORM;
	}
	log_message(LOG_INFO, "%s", platform_reason);

	lock_fd = acquire_lock();
	if (lock_fd < 0) {
		fprintf(stderr, "cannot acquire watchdog lock: %s\n", strerror(-lock_fd));
		return 1;
	}
	sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	signal(SIGPIPE, SIG_IGN);

	result = monitor_journal();
	close(lock_fd);
	return result < 0 ? 1 : 0;
}
