/*
 * Ask iOS where ABM's personalized baseband firmware is, the way BootControllerLegacy::prepare does
 * (libBasebandManager -> capabilities::bb::radio::getPersonalizedFirmwarePath -> MSUDataAccessor
 * lookupPathForPersonalizedData(1, ...), then + "/Mav17"). Run it on the phone, as root or as _wireless:
 *     msu-path-probe            -> "type N: error 0x...." or "type N: <path> (exists|missing)"
 * Errors from MSUDataAccessor: 0x1770 bad arguments, 0x1771 no path/copy failed, 0x1773 preboot volume not
 * found, 0x1775 boot manifest hash not found (COMMCENTER-umi.md). Prints paths only, no identities.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int
main(void)
{
	void *msu = dlopen("/System/Library/PrivateFrameworks/MSUDataAccessor.framework/MSUDataAccessor", RTLD_NOW);
	long (*lookup)(int, char *, size_t) = msu ? (long (*)(int, char *, size_t))dlsym(msu, "lookupPathForPersonalizedData") : NULL;
	if (!lookup) {
		printf("MSUDataAccessor: %s\n", dlerror());
		return 1;
	}
	for (int type = 0; type < 4; type++) {
		char path[1024];
		memset(path, 0, sizeof(path));
		long error = lookup(type, path, sizeof(path));
		if (error) {
			printf("type %d: error 0x%lx\n", type, error);
			continue;
		}
		struct stat st;
		char mav[1100];
		snprintf(mav, sizeof(mav), "%s/Mav17", path);
		printf("type %d: %s (%s); %s (%s)\n", type, path, stat(path, &st) == 0 ? "exists" : "missing", mav,
		    stat(mav, &st) == 0 ? "exists" : "missing");
	}
	return 0;
}
