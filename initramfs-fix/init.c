/* initramfs /init — one-shot repair hook for the Korvo-1 rootfs.
 *
 * The X11 rootfs was laid down by untarring over the running system; the
 * musl loader /lib/ld-musl-riscv32-sp.so.1 was recreated through the
 * serial console and came out mode 0644, so every dynamic binary on the
 * SD root fails execve with EACCES and the kernel panics at "Starting
 * init".  This static init mounts the SD root, fixes the loader bits,
 * moves the mount to / and execs the real init.
 */
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

static void die(const char *what)
{
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        dprintf(fd, "initramfs-init: %s failed\n", what);
        close(fd);
    }
    for (;;)
        sleep(3600);
}

static int wait_blockdev(const char *path)
{
    int i;

    /* The SD host can probe after initramfs /init starts.  Wait for the
     * block device instead of racing it; the normal root= path had
     * rootwait, but an embedded /init bypasses that mechanism. */
    for (i = 0; i < 200; i++) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        usleep(50000);
    }
    return -1;
}

int main(void)
{
    mkdir("/dev", 0755);
    mkdir("/mnt", 0755);
    /* device nodes (rootfs is devtmpfs-less here; create statically) */
    mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
    mknod("/dev/mmcblk0p2", S_IFBLK | 0600, makedev(179, 2));
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));

    if (wait_blockdev("/dev/mmcblk0p2"))
        die("wait /dev/mmcblk0p2");
    if (mount("/dev/mmcblk0p2", "/mnt", "ext4", 0, 0))
        die("mount");
    /* CONFIG_DEVTMPFS_MOUNT is skipped when an initramfs /init is used.
     * Attach devtmpfs to the real root before moving it over /. */
    if (mount("devtmpfs", "/mnt/dev", "devtmpfs", 0, "mode=0755"))
        die("devtmpfs");
    /* the loader (musl ld = libc) and anything else that might have been
     * recreated through the console need the exec bit */
    chmod("/mnt/lib/ld-musl-riscv32-sp.so.1", 0755);
    chmod("/mnt/lib/libc.so", 0755);
    chmod("/mnt/bin/busybox", 0755);
    chmod("/mnt/sbin/init", 0755);

    if (chdir("/mnt"))
        die("chdir");
    if (mount(".", "/", NULL, MS_MOVE, 0))
        die("move");
    if (chroot("."))
        die("chroot");
    execl("/sbin/init", "init", (char *)0);
    execl("/bin/busybox", "init", (char *)0);
    die("exec");
    return 1;
}
