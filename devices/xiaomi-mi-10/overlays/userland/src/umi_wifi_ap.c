/* Explicit open-network AP proof control. No background service, NAT or DHCP.
 * Config file: SSID on line 1, channel (1..11) on line 2. Additional lines are
 * rejected: a passphrase must never silently become an open AP. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define AP_FILE "/var/db/hakios-hotspot/wifi-ap"
#define AP_SYSCTL "kern.hakios.wifi_ap"

/* Darwin's SIOCAIFADDR IPv4 ABI; in_var.h is not in the public iOS SDK. */
struct ap_ifaliasreq {
	char name[IFNAMSIZ];
	struct sockaddr_in address, broadcast, mask;
};
#define AP_SIOCAIFADDR _IOW('i', 26, struct ap_ifaliasreq)

static int
set_mode(const char *mode)
{
	if (!sysctlbyname(AP_SYSCTL, NULL, NULL, (void *)mode, strlen(mode) + 1)) return 0;
	fprintf(stderr, "hotspot-ap: control failed: %s\n", strerror(errno));
	return -1;
}

static struct sockaddr_in
ip_address(const char *address)
{
	struct sockaddr_in result = { .sin_len = sizeof(result), .sin_family = AF_INET };
	inet_pton(AF_INET, address, &result.sin_addr);
	return result;
}

static int
set_address(int add)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0), ret;
	if (fd < 0) return -1;
	struct ap_ifaliasreq request = { 0 };
	strlcpy(request.name, "ap1", sizeof(request.name));
	request.address = ip_address("192.168.2.1");
	request.broadcast = ip_address("192.168.2.255");
	request.mask = ip_address("255.255.255.0");
	ret = ioctl(fd, add ? AP_SIOCAIFADDR : SIOCDIFADDR, &request);
	if (!ret && add) {
		struct ifreq flags = { 0 };
		strlcpy(flags.ifr_name, "ap1", sizeof(flags.ifr_name));
		ret = ioctl(fd, SIOCGIFFLAGS, &flags);
		if (!ret) {
			flags.ifr_flags |= IFF_UP;
			ret = ioctl(fd, SIOCSIFFLAGS, &flags);
		}
	}
	int saved = errno;
	close(fd);
	if (ret && !add && (saved == EADDRNOTAVAIL || saved == ENXIO || saved == ENODEV)) return 0;
	if (ret) fprintf(stderr, "hotspot-ap: ap1 address failed: %s\n", strerror(saved));
	return ret;
}

static int
parse_config(char *data, size_t length, char *mode, size_t capacity)
{
	if (!length || length >= 80) goto invalid;
	data[length] = 0;
	if (memchr(data, 0, (size_t)length)) goto invalid;
	char *newline = strchr(data, '\n');
	if (!newline) goto invalid;
	*newline++ = 0;
	size_t ssid_length = strlen(data);
	if (!ssid_length || ssid_length > 32) goto invalid;
	for (size_t i = 0; i < ssid_length; i++)
		if ((unsigned char)data[i] < 0x20 || (unsigned char)data[i] > 0x7e) goto invalid;
	/* Do not let strtol skip whitespace/newlines into a third line. */
	char *end = newline;
	unsigned channel = 0;
	while (*end >= '0' && *end <= '9' && end - newline < 2)
		channel = channel * 10 + (unsigned)(*end++ - '0');
	if (end == newline || channel < 1 || channel > 11 || (*end && strcmp(end, "\n"))) goto invalid;
	if ((size_t)snprintf(mode, capacity, "%u %s", channel, data) >= capacity) goto invalid;
	return 0;
invalid:
	fprintf(stderr, "hotspot-ap: invalid config; expected SSID then channel, exactly two lines (open only)\n");
	return -1;
}

static int
read_config(const char *path, char *mode, size_t capacity)
{
	char data[80];
	struct stat st;
	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0) { fprintf(stderr, "hotspot-ap: config open failed: %s\n", strerror(errno)); return -1; }
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022)) {
		fprintf(stderr, "hotspot-ap: config must be a root-owned regular file without group/other write\n");
		close(fd); return -1;
	}
	ssize_t length = read(fd, data, sizeof(data));
	close(fd);
	return parse_config(data, length > 0 ? (size_t)length : 0, mode, capacity);
}

int
main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--status")) {
		char mode[40]; size_t length = sizeof(mode);
		if (sysctlbyname(AP_SYSCTL, mode, &length, NULL, 0)) {
			fprintf(stderr, "hotspot-ap: status failed: %s\n", strerror(errno)); return 1;
		}
		printf("hotspot-ap: %s\n", mode); return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "--off")) {
		int error = set_mode("off");
		if (!error) error = set_address(0);
		return error ? 1 : 0;
	}
	if (argc >= 2 && argc <= 3 && !strcmp(argv[1], "--apply")) {
		char mode[40];
		if (read_config(argc == 3 ? argv[2] : AP_FILE, mode, sizeof(mode))) return 1;
		if (set_mode(mode)) return 1;
		if (set_address(1)) { set_mode("off"); set_address(0); return 1; }
		printf("hotspot-ap: ap1 192.168.2.1/24 configured; open AP, one client\n");
		return 0;
	}
	fprintf(stderr, "usage: umi_wifi_ap --apply [file] | --off | --status\n");
	return 2;
}
