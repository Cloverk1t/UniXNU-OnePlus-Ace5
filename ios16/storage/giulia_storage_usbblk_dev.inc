/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Zero-write BSD block/char device (/dev/{r,}waffleusb0) backed by Mac host
 * over DWC3 USB Bulk (EP3 OUT/IN). Touches zero UFS registers.
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
/* GIULIA_USBBLK_PROTO is embedded here by the profile generator. */

extern int waffle_usbblk_enabled(void);
extern int waffle_usbblk_xfer(uint32_t op, uint64_t lba, uint32_t count, void *buf,
                              uint32_t *out_status, uint64_t *out_blocks, uint32_t *out_flags);
extern void waffle_usbblk_stats(uint64_t *ok, uint64_t *err, uint64_t *rx, uint64_t *tx, unsigned *ready);

static LCK_GRP_DECLARE(wub_dev_grp, "waffle_usbblk_dev");
static LCK_MTX_DECLARE(wub_dev_lock, &wub_dev_grp);

static struct {
    uint64_t total_blocks;
    uint32_t secsize;
    bool ready, failed, writable, root_valid;
    int bmajor, cmajor;
    uint8_t bounce[WUB_MAX_DATA_BYTES];
} wubd;

static uint64_t wubd_bytes(void)
{
    return wubd.total_blocks * (uint64_t)WUB_BLOCK_BYTES;
}

static int wubd_open(dev_t dev, int flags, __unused int type, __unused struct proc *p)
{
    if (minor(dev) || !wubd.ready || wubd.failed) return ENXIO;
    return (flags & FWRITE) && !wubd.writable ? EROFS : 0;
}

static int wubd_close(__unused dev_t d, __unused int f, __unused int t, __unused struct proc *p)
{
    if (!wubd.writable || wubd.failed) return 0;
    uint32_t st = 0;
    int err = waffle_usbblk_xfer(WUB_OP_SYNC, 0, 0, NULL, &st, NULL, NULL);
    return err ? err : (st ? EIO : 0);
}

static void wubd_strategy(buf_t bp)
{
    uint64_t off = 0;
    uint32_t count = buf_count(bp);
    caddr_t va = NULL;
    int error = 0;
    buf_setresid(bp, count);
    bool is_read = (buf_flags(bp) & B_READ) != 0;
    if (!is_read && !wubd.writable) { error = EROFS; goto done; }
    if (buf_flags(bp) & B_PHYS) { error = EINVAL; goto done; }
    if (!wubd.ready || wubd.failed || minor(buf_device(bp))) { error = ENXIO; goto done; }
    if (!count) goto done;
    if (buf_map(bp, &va)) { error = EIO; goto done; }

    lck_mtx_lock(&wub_dev_lock);
    if (wub_io_bounds(buf_blkno(bp), wubd.secsize, count, wubd_bytes(), &off)) {
        error = EINVAL;
        goto unlock;
    }
    for (unsigned copied = 0; copied < count;) {
        uint64_t pos = off + copied;
        unsigned skip = (unsigned)(pos % WUB_BLOCK_BYTES);
        unsigned want = MIN(count - copied, WUB_MAX_DATA_BYTES - skip);
        unsigned blocks = (skip + want + WUB_BLOCK_BYTES - 1) / WUB_BLOCK_BYTES;
        uint64_t lba = pos / WUB_BLOCK_BYTES;
        uint32_t st = 0;
        if (is_read || skip || (want % WUB_BLOCK_BYTES)) {
            error = waffle_usbblk_xfer(WUB_OP_READ, lba, blocks, wubd.bounce, &st, NULL, NULL);
            if (!error && st) error = EIO;
            if (error) break;
        }
        if (is_read) {
            memcpy(va + copied, wubd.bounce + skip, want);
        } else {
            memcpy(wubd.bounce + skip, va + copied, want);
            error = waffle_usbblk_xfer(WUB_OP_WRITE, lba, blocks, wubd.bounce, &st, NULL, NULL);
            if (!error && st) error = EIO;
            if (error) break;
        }
        copied += want;
        buf_setresid(bp, count - copied);
    }
unlock:
    lck_mtx_unlock(&wub_dev_lock);
    buf_unmap(bp);
done:
    if (error) buf_seterror(bp, error);
    buf_biodone(bp);
}

static int wubd_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int flags,
                      __unused struct proc *p, bool ischar)
{
    if (minor(dev) || !wubd.ready || wubd.failed) return ENXIO;
    uint32_t *f = (uint32_t *)data;
    uint64_t *n = (uint64_t *)data;
    switch (cmd) {
    case DKIOCGETBLOCKSIZE: *f = wubd.secsize; return 0;
    case DKIOCSETBLOCKSIZE:
        if (ischar) return ENODEV;
        if (*f < 512 || *f > WUB_BLOCK_BYTES || (*f & (*f - 1))) return EINVAL;
        lck_mtx_lock(&wub_dev_lock);
        wubd.secsize = *f;
        lck_mtx_unlock(&wub_dev_lock);
        return 0;
    case DKIOCGETPHYSICALBLOCKSIZE: *f = WUB_BLOCK_BYTES; return 0;
    case DKIOCGETBLOCKCOUNT: *n = wubd_bytes() / wubd.secsize; return 0;
    case DKIOCISWRITABLE: *f = wubd.writable; return 0;
    case DKIOCGETMAXBLOCKCOUNTREAD: *n = WUB_MAX_DATA_BYTES / wubd.secsize; return 0;
    case DKIOCGETMAXBLOCKCOUNTWRITE: *n = wubd.writable ? (WUB_MAX_DATA_BYTES / wubd.secsize) : 0; return 0;
    case DKIOCSYNCHRONIZE:
    case DKIOCSYNCHRONIZECACHE: {
        if (!wubd.writable) return 0;
        uint32_t st = 0;
        int err = waffle_usbblk_xfer(WUB_OP_SYNC, 0, 0, NULL, &st, NULL, NULL);
        return err ? err : (st ? EIO : 0);
    }
    case DKIOCGETMAXSEGMENTCOUNTREAD:
    case DKIOCGETMAXSEGMENTCOUNTWRITE: *n = 1; return 0;
    default: return ENOTTY;
    }
}

static int wubd_bioctl(dev_t d, u_long c, caddr_t a, int f, struct proc *p)
{ return wubd_ioctl(d, c, a, f, p, false); }
static int wubd_cioctl(dev_t d, u_long c, caddr_t a, int f, struct proc *p)
{ return wubd_ioctl(d, c, a, f, p, true); }
static int wubd_psize(__unused dev_t d)
{ return wubd.ready && !wubd.failed ? (int)wubd.secsize : -1; }

static int wubd_read(dev_t d, struct uio *u_io, __unused int f)
{
    if (minor(d) || !wubd.ready || wubd.failed) return ENXIO;
    int64_t offset = uio_offset(u_io), residual = uio_resid(u_io);
    uint64_t total = wubd_bytes();
    if (offset < 0 || residual < 0 || (uint64_t)offset > total) return EINVAL;
    int error = 0;
    lck_mtx_lock(&wub_dev_lock);
    while (uio_resid(u_io) > 0 && (uint64_t)uio_offset(u_io) < total) {
        uint64_t pos = (uint64_t)uio_offset(u_io);
        unsigned skip = (unsigned)(pos % WUB_BLOCK_BYTES);
        unsigned want = (unsigned)MIN((uint64_t)uio_resid(u_io), WUB_MAX_DATA_BYTES - skip);
        want = (unsigned)MIN((uint64_t)want, total - pos);
        unsigned blocks = (skip + want + WUB_BLOCK_BYTES - 1) / WUB_BLOCK_BYTES;
        uint32_t st = 0;
        error = waffle_usbblk_xfer(WUB_OP_READ, pos / WUB_BLOCK_BYTES, blocks, wubd.bounce, &st, NULL, NULL);
        if (!error && st) error = EIO;
        if (error) break;
        error = uiomove((caddr_t)(wubd.bounce + skip), (int)want, u_io);
        if (error) break;
    }
    lck_mtx_unlock(&wub_dev_lock);
    return error;
}

static int wubd_write(dev_t d, struct uio *u_io, __unused int f)
{
    if (!wubd.writable) return EROFS;
    if (minor(d) || !wubd.ready || wubd.failed) return ENXIO;
    int64_t offset = uio_offset(u_io), residual = uio_resid(u_io);
    uint64_t total = wubd_bytes();
    if (offset < 0 || residual < 0 || (uint64_t)offset > total ||
        (uint64_t)residual > total - (uint64_t)offset) return EINVAL;
    int error = 0;
    lck_mtx_lock(&wub_dev_lock);
    while (uio_resid(u_io) > 0) {
        uint64_t pos = (uint64_t)uio_offset(u_io);
        unsigned skip = (unsigned)(pos % WUB_BLOCK_BYTES);
        unsigned want = (unsigned)MIN((uint64_t)uio_resid(u_io), WUB_MAX_DATA_BYTES - skip);
        unsigned blocks = (skip + want + WUB_BLOCK_BYTES - 1) / WUB_BLOCK_BYTES;
        uint32_t st = 0;
        if (skip || (want % WUB_BLOCK_BYTES)) {
            error = waffle_usbblk_xfer(WUB_OP_READ, pos / WUB_BLOCK_BYTES, blocks, wubd.bounce, &st, NULL, NULL);
            if (!error && st) error = EIO;
            if (error) break;
        }
        error = uiomove((caddr_t)(wubd.bounce + skip), (int)want, u_io);
        if (error) break;
        error = waffle_usbblk_xfer(WUB_OP_WRITE, pos / WUB_BLOCK_BYTES, blocks, wubd.bounce, &st, NULL, NULL);
        if (!error && st) error = EIO;
        if (error) break;
    }
    lck_mtx_unlock(&wub_dev_lock);
    return error;
}

static const struct bdevsw wubd_bdevsw = {
    .d_open = wubd_open, .d_close = wubd_close, .d_strategy = wubd_strategy,
    .d_ioctl = wubd_bioctl, .d_dump = eno_dump, .d_psize = wubd_psize, .d_type = D_DISK,
};

static const struct cdevsw wubd_cdevsw = {
    .d_open = wubd_open, .d_close = wubd_close, .d_read = wubd_read, .d_write = wubd_write,
    .d_ioctl = wubd_cioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_ttys = NULL,
    .d_select = eno_select, .d_mmap = eno_mmap, .d_strategy = eno_strat,
    .d_reserved_1 = eno_getc, .d_reserved_2 = eno_putc, .d_type = D_DISK,
};

static int wubd_sleep_chan;
static void wubd_sleep_ms(unsigned ms)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
    (void)assert_wait_deadline(&wubd_sleep_chan, THREAD_UNINT, deadline);
    thread_block(THREAD_CONTINUE_NULL);
}

static int waffle_usbblk_wait_info(uint64_t *blocks_out, uint32_t *flags_out)
{
    for (unsigned attempt = 1; attempt <= 120; attempt++) {
        uint32_t st = 0, flags = 0;
        uint64_t blocks = 0;
        unsigned ready = 0;
        waffle_usbblk_stats(NULL, NULL, NULL, NULL, &ready);
        if (!ready) {
            if (attempt == 1 || (attempt % 5) == 0)
                printf("WAFFLE USBBLK: awaiting USB enumeration (%u/120)...\n", attempt);
            wubd_sleep_ms(1000);
            continue;
        }
        int err = waffle_usbblk_xfer(WUB_OP_INFO, 0, 0, NULL, &st, &blocks, &flags);
        if (!err && !st && blocks > 0) {
            *blocks_out = blocks;
            *flags_out = flags;
            return 0;
        }
        printf("WAFFLE USBBLK: awaiting host daemon INFO (%u/120 err=%d st=%u)\n", attempt, err, st);
        wubd_sleep_ms(500);
    }
    return ETIMEDOUT;
}

int waffle_usbblk_selftest(void);
int waffle_usbblk_selftest(void)
{
    uint64_t blocks = 0;
    uint32_t flags = 0;
    printf("WAFFLE USBBLK: zero-write USB Bulk block selftest starting (UFS untouched)\n");
    int err = waffle_usbblk_wait_info(&blocks, &flags);
    if (err) {
        printf("WAFFLE USBBLK: selftest INFO failed err=%d\n", err);
        return err;
    }
    printf("WAFFLE USBBLK: host connected blocks=%llu (%llu MiB) writable=%u\n",
           blocks, (blocks * 4096ULL) >> 20, flags & WUB_FLAG_WRITABLE);
    uint64_t to_read = blocks < 256 ? blocks : 256;
    uint64_t t0_ns = 0, t1_ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &t0_ns);
    uint32_t lba0_crc = 0;
    for (uint64_t lba = 0; lba < to_read;) {
        uint32_t chunk = (uint32_t)((to_read - lba) > WUB_MAX_BLOCKS ? WUB_MAX_BLOCKS : (to_read - lba));
        uint32_t st = 0;
        err = waffle_usbblk_xfer(WUB_OP_READ, lba, chunk, wubd.bounce, &st, NULL, NULL);
        if (err || st) {
            printf("WAFFLE USBBLK: selftest read failed lba=%llu err=%d st=%u\n", lba, err, st);
            return err ? err : EIO;
        }
        if (lba == 0) lba0_crc = wub_crc32(wubd.bounce, WUB_BLOCK_BYTES);
        lba += chunk;
    }
    absolutetime_to_nanoseconds(mach_absolute_time(), &t1_ns);
    uint64_t ms = (t1_ns > t0_ns) ? (t1_ns - t0_ns) / 1000000ULL : 1ULL;
    if (!ms) ms = 1;
    uint64_t kbps = (to_read * 4ULL * 1000ULL) / ms;
    printf("WAFFLE USBBLK: SELFTEST PASS blocks=%llu lba0_crc=%08x elapsed=%llums speed=%lluKB/s\n",
           to_read, lba0_crc, ms, kbps);
    return 0;
}

int waffle_usbblk_ios_rootdev(dev_t *dev);
int waffle_usbblk_ios_rootdev(dev_t *dev)
{
    if (wubd.ready && !wubd.failed) {
        *dev = makedev(wubd.bmajor, 0);
        return 0;
    }
    uint64_t blocks = 0;
    uint32_t flags = 0;
    int err = waffle_usbblk_wait_info(&blocks, &flags);
    if (err) return err;

    uint32_t st = 0;
    err = waffle_usbblk_xfer(WUB_OP_READ, 0, 1, wubd.bounce, &st, NULL, NULL);
    if (err || st) return err ? err : EIO;

    char uuid_text[17] = {0};
    uint64_t uuid = 0;
    int rw_arg = 0;
    PE_parse_boot_argn("vermeer_root_uuid", uuid_text, sizeof(uuid_text));
    PE_parse_boot_argn("waffle_usbblk_rw", &rw_arg, sizeof(rw_arg));
    if (uuid_text[0] && strcmp(uuid_text, "any") != 0) {
        if (vi_uuid(uuid_text, &uuid) || !wub_hfs_root(wubd.bounce, blocks, uuid)) {
            printf("WAFFLE USBBLK: HFSX root header/UUID mismatch on host image\n");
            return EINVAL;
        }
    } else if (!wub_hfs_root(wubd.bounce, blocks, 0)) {
        printf("WAFFLE USBBLK: host image is not a valid 4K journaled HFSX root\n");
        return EINVAL;
    }

    wubd.total_blocks = blocks;
    wubd.secsize = 512;
    wubd.root_valid = true;
    wubd.writable = (rw_arg == 1) && ((flags & WUB_FLAG_WRITABLE) != 0);

    wubd.bmajor = bdevsw_add(-1, &wubd_bdevsw);
    if (wubd.bmajor < 0) return ENXIO;
    wubd.cmajor = cdevsw_add_with_bdev(-1, &wubd_cdevsw, wubd.bmajor);
    if (wubd.cmajor < 0) return ENXIO;
    void *bn = devfs_make_node(makedev(wubd.bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR,
                               wubd.writable ? 0600 : 0400, "waffleusb0");
    void *cn = devfs_make_node(makedev(wubd.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR,
                               wubd.writable ? 0600 : 0400, "rwaffleusb0");
    if (!bn || !cn) return ENXIO;
    wubd.ready = true;
    printf("WAFFLE USBBLK: /dev/{r,}waffleusb0 ready (%llu MiB, %s, UFS untouched)\n",
           (blocks * 4096ULL) >> 20, wubd.writable ? "READ-WRITE" : "READ ONLY");
    *dev = makedev(wubd.bmajor, 0);
    return 0;
}
