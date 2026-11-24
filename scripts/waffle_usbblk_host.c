/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Native macOS IOKit USB Bulk host-backed block server for OnePlus 12 (waffle).
 * Zero third-party dependencies. Build on macOS:
 *   clang -O2 -std=c11 -Wall -Wextra -Werror \
 *     scripts/waffle_usbblk_host.c -framework IOKit -framework CoreFoundation \
 *     -o /tmp/waffle-usbblk-host
 *
 * Usage:
 *   /tmp/waffle-usbblk-host --image path/to/ios16.img [--ro | --cow | --rw]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../ios16/storage/giulia_storage_usbblk_proto.h"

enum host_mode { MODE_RO = 0, MODE_COW = 1, MODE_RW = 2 };

struct cow_entry {
    uint64_t lba;
    uint8_t *data;
    struct cow_entry *next;
};

#define COW_BUCKETS 16384u
struct block_store {
    int fd;
    uint64_t total_blocks;
    enum host_mode mode;
    struct cow_entry *buckets[COW_BUCKETS];
    uint64_t cow_blocks;
};

static volatile sig_atomic_t g_stop;
static void on_sig(int sig) { (void)sig; g_stop = 1; }

static uint8_t *cow_find(struct block_store *s, uint64_t lba)
{
    for (struct cow_entry *e = s->buckets[lba % COW_BUCKETS]; e; e = e->next)
        if (e->lba == lba) return e->data;
    return NULL;
}

static int cow_put(struct block_store *s, uint64_t lba, const uint8_t *buf)
{
    uint8_t *existing = cow_find(s, lba);
    if (existing) {
        memcpy(existing, buf, WUB_BLOCK_BYTES);
        return 0;
    }
    struct cow_entry *e = (struct cow_entry *)malloc(sizeof(*e));
    uint8_t *data = (uint8_t *)malloc(WUB_BLOCK_BYTES);
    if (!e || !data) { free(e); free(data); return ENOMEM; }
    memcpy(data, buf, WUB_BLOCK_BYTES);
    e->lba = lba;
    e->data = data;
    e->next = s->buckets[lba % COW_BUCKETS];
    s->buckets[lba % COW_BUCKETS] = e;
    s->cow_blocks++;
    return 0;
}

static void cow_free(struct block_store *s)
{
    for (unsigned i = 0; i < COW_BUCKETS; i++) {
        struct cow_entry *e = s->buckets[i];
        while (e) {
            struct cow_entry *n = e->next;
            free(e->data);
            free(e);
            e = n;
        }
        s->buckets[i] = NULL;
    }
    s->cow_blocks = 0;
}

static int store_read(struct block_store *s, uint64_t lba, uint32_t count, uint8_t *out)
{
    if (!count || count > WUB_MAX_BLOCKS || lba >= s->total_blocks ||
        count > s->total_blocks - lba)
        return EINVAL;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *dst = out + (size_t)i * WUB_BLOCK_BYTES;
        uint8_t *cached = (s->mode == MODE_COW) ? cow_find(s, lba + i) : NULL;
        if (cached) {
            memcpy(dst, cached, WUB_BLOCK_BYTES);
            continue;
        }
        off_t off = (off_t)((lba + i) * WUB_BLOCK_BYTES);
        ssize_t got = pread(s->fd, dst, WUB_BLOCK_BYTES, off);
        if (got != (ssize_t)WUB_BLOCK_BYTES) return EIO;
    }
    return 0;
}

static int store_write(struct block_store *s, uint64_t lba, uint32_t count, const uint8_t *in)
{
    if (s->mode == MODE_RO) return EROFS;
    if (!count || count > WUB_MAX_BLOCKS || lba >= s->total_blocks ||
        count > s->total_blocks - lba)
        return EINVAL;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *src = in + (size_t)i * WUB_BLOCK_BYTES;
        if (s->mode == MODE_COW) {
            int err = cow_put(s, lba + i, src);
            if (err) return err;
        } else {
            off_t off = (off_t)((lba + i) * WUB_BLOCK_BYTES);
            ssize_t put = pwrite(s->fd, src, WUB_BLOCK_BYTES, off);
            if (put != (ssize_t)WUB_BLOCK_BYTES) return EIO;
        }
    }
    return 0;
}

static int open_usb_interface(IOUSBInterfaceInterface300 ***out_intf,
                              uint8_t *out_pipe_in, uint8_t *out_pipe_out)
{
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBInterfaceClassName);
    if (!match) return -1;
    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter);
    if (kr != KERN_SUCCESS) return -1;

    int found = -1;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter)) != 0) {
        IOCFPlugInInterface **plugin = NULL;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID,
                                              kIOCFPlugInInterfaceID, &plugin, &score) == KERN_SUCCESS && plugin) {
            IOUSBInterfaceInterface300 **intf = NULL;
            HRESULT hr = (*plugin)->QueryInterface(plugin,
                CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&intf);
            (*plugin)->Release(plugin);
            if (hr == S_OK && intf) {
                UInt16 vid = 0, pid = 0;
                UInt8 cls = 0, sub = 0, proto = 0, num_eps = 0;
                (*intf)->GetDeviceVendor(intf, &vid);
                (*intf)->GetDeviceProduct(intf, &pid);
                (*intf)->GetInterfaceClass(intf, &cls);
                (*intf)->GetInterfaceSubClass(intf, &sub);
                (*intf)->GetInterfaceProtocol(intf, &proto);
                if (vid == WUB_VID && pid == WUB_PID &&
                    cls == WUB_IF_CLASS && sub == WUB_IF_SUBCLASS && proto == WUB_IF_PROTOCOL) {
                    if ((*intf)->USBInterfaceOpen(intf) == kIOReturnSuccess) {
                        (*intf)->GetNumEndpoints(intf, &num_eps);
                        uint8_t pin = 0, pout = 0;
                        for (uint8_t p = 1; p <= num_eps; p++) {
                            UInt8 dir = 0, num = 0, xfer = 0, ivl = 0;
                            UInt16 mps = 0;
                            if ((*intf)->GetPipeProperties(intf, p, &dir, &num, &xfer, &mps, &ivl) == kIOReturnSuccess &&
                                xfer == kUSBBulk) {
                                if (dir == kUSBIn && num == (WUB_EP_IN_ADDR & 0x0f)) pin = p;
                                if (dir == kUSBOut && num == (WUB_EP_OUT_ADDR & 0x0f)) pout = p;
                            }
                        }
                        if (pin && pout) {
                            *out_intf = intf;
                            *out_pipe_in = pin;
                            *out_pipe_out = pout;
                            found = 0;
                            IOObjectRelease(svc);
                            break;
                        }
                        (*intf)->USBInterfaceClose(intf);
                    }
                }
                (*intf)->Release(intf);
            }
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(iter);
    return found;
}

static int read_exact(IOUSBInterfaceInterface300 **intf, uint8_t pipe, uint8_t *buf, uint32_t want)
{
    uint32_t got = 0;
    while (got < want && !g_stop) {
        UInt32 n = want - got;
        IOReturn r = (*intf)->ReadPipeTO(intf, pipe, buf + got, &n, 5000, 10000);
        if (r == kIOUSBTransactionTimeout) continue;
        if (r != kIOReturnSuccess) return -1;
        got += n;
    }
    return got == want ? 0 : -1;
}

static int write_exact(IOUSBInterfaceInterface300 **intf, uint8_t pipe, const uint8_t *buf, uint32_t len)
{
    IOReturn r = (*intf)->WritePipeTO(intf, pipe, (void *)buf, len, 5000, 10000);
    return r == kIOReturnSuccess ? 0 : -1;
}

static void serve_session(struct block_store *store, IOUSBInterfaceInterface300 **intf,
                          uint8_t pipe_in, uint8_t pipe_out)
{
    uint8_t hdr_in[WUB_HDR_BYTES], hdr_out[WUB_HDR_BYTES];
    uint8_t data[WUB_MAX_DATA_BYTES];
    uint64_t ops = 0, rd_bytes = 0, wr_bytes = 0;
    time_t last_report = time(NULL);

    fprintf(stderr, "[usbblk] connected to OnePlus 12 (blocks=%" PRIu64 ", %.1f MiB, mode=%s)\n",
            store->total_blocks, (double)(store->total_blocks * WUB_BLOCK_BYTES) / (1024.0 * 1024.0),
            store->mode == MODE_RO ? "ro" : (store->mode == MODE_COW ? "cow" : "rw"));

    while (!g_stop) {
        UInt32 n = WUB_HDR_BYTES;
        IOReturn r = (*intf)->ReadPipeTO(intf, pipe_in, hdr_in, &n, 1000, 2000);
        if (r == kIOUSBTransactionTimeout) continue;
        if (r != kIOReturnSuccess) {
            fprintf(stderr, "[usbblk] disconnected (IOReturn=0x%x)\n", (unsigned)r);
            break;
        }
        if (n != WUB_HDR_BYTES) {
            fprintf(stderr, "[usbblk] short header (%u bytes); resetting session\n", (unsigned)n);
            break;
        }
        struct wub_req req;
        if (!wub_decode_req(hdr_in, &req)) {
            fprintf(stderr, "[usbblk] bad request header/CRC\n");
            break;
        }
        struct wub_rsp rsp = {0};
        rsp.seq = req.seq;
        uint32_t data_bytes = req.count * WUB_BLOCK_BYTES;

        if (req.op == WUB_OP_INFO) {
            rsp.status = 0;
            rsp.block_size = WUB_BLOCK_BYTES;
            rsp.total_blocks = store->total_blocks;
            rsp.data_crc = (store->mode != MODE_RO) ? WUB_FLAG_WRITABLE : 0u;
        } else if (req.op == WUB_OP_READ) {
            int err = store_read(store, req.lba, req.count, data);
            rsp.status = (uint32_t)err;
            rsp.block_size = err ? 0u : data_bytes;
            rsp.data_crc = err ? 0u : wub_crc32(data, data_bytes);
            if (!err) rd_bytes += data_bytes;
        } else if (req.op == WUB_OP_WRITE) {
            if (read_exact(intf, pipe_in, data, data_bytes) != 0) {
                fprintf(stderr, "[usbblk] failed receiving write data\n");
                break;
            }
            if (wub_crc32(data, data_bytes) != req.data_crc) {
                rsp.status = EIO;
            } else {
                int err = store_write(store, req.lba, req.count, data);
                rsp.status = (uint32_t)err;
                if (!err) wr_bytes += data_bytes;
            }
        } else if (req.op == WUB_OP_SYNC) {
            if (store->mode == MODE_RW && fsync(store->fd) != 0) rsp.status = (uint32_t)errno;
        } else {
            rsp.status = EINVAL;
        }

        wub_encode_rsp(hdr_out, &rsp);
        if (write_exact(intf, pipe_out, hdr_out, WUB_HDR_BYTES) != 0) {
            fprintf(stderr, "[usbblk] failed sending response header\n");
            break;
        }
        if (req.op == WUB_OP_READ && rsp.status == 0) {
            if (write_exact(intf, pipe_out, data, data_bytes) != 0) {
                fprintf(stderr, "[usbblk] failed sending read payload\n");
                break;
            }
        }
        ops++;
        time_t now = time(NULL);
        if (now - last_report >= 5) {
            fprintf(stderr, "[usbblk] ops=%" PRIu64 " read=%.1f MiB write=%.1f MiB cow_blocks=%" PRIu64 "\n",
                    ops, (double)rd_bytes / (1024.0 * 1024.0),
                    (double)wr_bytes / (1024.0 * 1024.0), store->cow_blocks);
            last_report = now;
        }
    }
    fprintf(stderr, "[usbblk] session ended: ops=%" PRIu64 " read=%.1f MiB write=%.1f MiB cow_blocks=%" PRIu64 "\n",
            ops, (double)rd_bytes / (1024.0 * 1024.0),
            (double)wr_bytes / (1024.0 * 1024.0), store->cow_blocks);
}

int main(int argc, char **argv)
{
    const char *image_path = NULL;
    enum host_mode mode = MODE_RO;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--image") && i + 1 < argc) image_path = argv[++i];
        else if (!strcmp(argv[i], "--ro")) mode = MODE_RO;
        else if (!strcmp(argv[i], "--cow")) mode = MODE_COW;
        else if (!strcmp(argv[i], "--rw")) mode = MODE_RW;
        else {
            fprintf(stderr, "Usage: %s --image <file.img> [--ro | --cow | --rw]\n", argv[0]);
            return 2;
        }
    }
    if (!image_path) {
        fprintf(stderr, "Missing --image <file.img>\n");
        return 2;
    }

    int flags = (mode == MODE_RW) ? O_RDWR : O_RDONLY;
    int fd = open(image_path, flags);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)WUB_BLOCK_BYTES ||
        (st.st_size % WUB_BLOCK_BYTES) != 0) {
        fprintf(stderr, "Image size must be a nonzero multiple of 4096 bytes\n");
        close(fd);
        return 1;
    }

    struct block_store store = {0};
    store.fd = fd;
    store.total_blocks = (uint64_t)st.st_size / WUB_BLOCK_BYTES;
    store.mode = mode;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    fprintf(stderr, "[usbblk] serving %s (%" PRIu64 " blocks, %.1f MiB, mode=%s); waiting for USB device...\n",
            image_path, store.total_blocks,
            (double)(store.total_blocks * WUB_BLOCK_BYTES) / (1024.0 * 1024.0),
            mode == MODE_RO ? "ro" : (mode == MODE_COW ? "cow" : "rw"));

    while (!g_stop) {
        IOUSBInterfaceInterface300 **intf = NULL;
        uint8_t pipe_in = 0, pipe_out = 0;
        if (open_usb_interface(&intf, &pipe_in, &pipe_out) == 0) {
            serve_session(&store, intf, pipe_in, pipe_out);
            (*intf)->USBInterfaceClose(intf);
            (*intf)->Release(intf);
            if (mode == MODE_COW) cow_free(&store);
            fprintf(stderr, "[usbblk] waiting for reconnect...\n");
        }
        usleep(250000);
    }
    cow_free(&store);
    close(fd);
    return 0;
}
