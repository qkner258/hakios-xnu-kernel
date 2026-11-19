/* File test only: refuses the RAM /var fallback and never opens a raw device. */
#include <sys/mount.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(void)
{
    struct statfs sf;
    struct timeval tv;
    const char *path = "/private/var/umi-flash-test.txt";
    char buf[4096];
    if (statfs("/private/var", &sf) != 0 || strcmp(sf.f_fstypename, "hfs") ||
        strcmp(sf.f_mntfromname, "/dev/ufs_darwin_data") || (sf.f_flags & MNT_RDONLY)) {
        puts("umi-data-test: FAIL: /var must be writable hfs from /dev/ufs_darwin_data");
        return 1;
    }
    printf("umi-data-test: %s on %s (%s)\n", sf.f_fstypename, sf.f_mntonname, sf.f_mntfromname);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) { close(fd); return 1; }
        buf[n] = 0;
        printf("umi-data-test: previous contents:\n%s", buf);
        close(fd);
    } else if (errno == ENOENT) {
        puts("umi-data-test: no previous marker (expected on first boot only)");
    } else { perror("read marker"); return 1; }
    gettimeofday(&tv, NULL);
    int n = snprintf(buf, sizeof(buf), "umi HFS persistence epoch=%lld\n", (long long)tv.tv_sec);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0 || write(fd, buf, n) != n || fsync(fd) != 0) {
        perror("append/fsync marker");
        if (fd >= 0) close(fd);
        return 1;
    }
    close(fd);
    sync();
    puts("umi-data-test: PASS: marker appended and fsync/sync completed; verify previous contents next boot");
    return 0;
}
