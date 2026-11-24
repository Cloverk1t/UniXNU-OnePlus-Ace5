/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * DWC3 EP6 (0x03 OUT) / EP7 (0x83 IN) transport for zero-write USB Bulk block I/O.
 * Included inside vermeer_usb_debug.c so usb_thread remains the sole DWC3 MMIO owner.
 */
#include <sys/errno.h>
#include <kern/locks.h>
/* GIULIA_USBBLK_PROTO is embedded here by the profile generator. */

#define WUB_DMA_SIZE     0x10000u /* 64 KiB permanent contiguous DMA window */
#define WUB_TRB_OUT_OFF  0x0000u  /* EP6 TRB */
#define WUB_TRB_IN_OFF   0x0040u  /* EP7 TRB */
#define WUB_HDR_IN_OFF   0x0100u  /* 32-byte request TX */
#define WUB_HDR_OUT_OFF  0x0140u  /* 512-byte response RX */
#define WUB_DATA_OFF     0x1000u  /* 32 KiB payload bounce buffer */

enum wub_state {
    WUB_IDLE = 0,
    WUB_QUEUED,
    WUB_SEND_REQ,
    WUB_SEND_DATA,
    WUB_RECV_RSP,
    WUB_RECV_DATA
};

static LCK_GRP_DECLARE(wub_lck_grp, "waffle_usbblk");
static LCK_MTX_DECLARE(wub_lck, &wub_lck_grp);
static LCK_MTX_DECLARE(wub_caller_lck, &wub_lck_grp);

static struct {
    vm_offset_t dma;
    uint64_t pa;
    uint32_t seq;
    unsigned ep_ready;
    int enabled;
    enum wub_state state;
    struct wub_req req;
    struct wub_rsp rsp;
    void *caller_buf;
    int done;
    int err;
    uint64_t deadline_ns;
    int wait_chan;
    uint64_t io_ok, io_err, rx_bytes, tx_bytes;
} wub_bus;

int waffle_usbblk_enabled(void);
int waffle_usbblk_xfer(uint32_t op, uint64_t lba, uint32_t count, void *buf,
                       uint32_t *out_status, uint64_t *out_blocks, uint32_t *out_flags);
void waffle_usbblk_stats(uint64_t *ok, uint64_t *err, uint64_t *rx, uint64_t *tx, unsigned *ready);

static uint64_t wub_now_ns(void)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

int waffle_usbblk_enabled(void)
{
    static int checked;
    if (!checked) {
        int en = 0;
        char rd_val[32] = {0};
        PE_parse_boot_argn("waffle_usbblk", &en, sizeof(en));
        PE_parse_boot_argn("rd", rd_val, sizeof(rd_val));
        if (en == 1 || !strcmp(rd_val, "waffleusb0")) wub_bus.enabled = 1;
        checked = 1;
    }
    return wub_bus.enabled;
}

void waffle_usbblk_stats(uint64_t *ok, uint64_t *err, uint64_t *rx, uint64_t *tx, unsigned *ready)
{
    lck_mtx_lock(&wub_lck);
    if (ok) *ok = wub_bus.io_ok;
    if (err) *err = wub_bus.io_err;
    if (rx) *rx = wub_bus.rx_bytes;
    if (tx) *tx = wub_bus.tx_bytes;
    if (ready) *ready = (u.configured && !u.failed && wub_bus.ep_ready) ? 1u : 0u;
    lck_mtx_unlock(&wub_lck);
}

static void wub_sync_dma(unsigned off, unsigned size)
{
    flush_dcache64((addr64_t)(wub_bus.pa + off), size, TRUE);
    __asm__ volatile("dsb sy" ::: "memory");
}

static void wub_init_dma(unsigned width)
{
    if (!waffle_usbblk_enabled()) return;
    uint32_t hw3 = rd(0xc14c);
    unsigned num_eps = (hw3 >> 12) & 0x3fu;
    unsigned num_in = (hw3 >> 18) & 0x1fu;
    if (num_eps < 8 || num_in < 4) {
        printf("WAFFLE USBBLK: insufficient DWC3 EPs (eps=%u in=%u)\n", num_eps, num_in);
        wub_bus.enabled = 0;
        return;
    }
    if (kmem_alloc_contig(kernel_map, &wub_bus.dma, WUB_DMA_SIZE, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
        printf("WAFFLE USBBLK: DMA alloc failed\n");
        wub_bus.enabled = 0;
        return;
    }
    ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)wub_bus.dma);
    if (!pn) { wub_bus.enabled = 0; return; }
    wub_bus.pa = ptoa_64(pn);
    if (wub_bus.pa < gPhysBase || gPhysSize < WUB_DMA_SIZE ||
        wub_bus.pa - gPhysBase > gPhysSize - WUB_DMA_SIZE ||
        (width < 64 && wub_bus.pa + WUB_DMA_SIZE - 1 >= (1ULL << width))) {
        printf("WAFFLE USBBLK: DMA range guard refused pa=%llx\n", wub_bus.pa);
        wub_bus.enabled = 0;
        return;
    }
    for (unsigned off = 0; off < WUB_DMA_SIZE; off += PAGE_SIZE) {
        ppnum_t page = pmap_find_phys(kernel_pmap, (addr64_t)(wub_bus.dma + off));
        if (!page || ptoa_64(page) != wub_bus.pa + off) {
            printf("WAFFLE USBBLK: non-contiguous DMA page\n");
            wub_bus.enabled = 0;
            return;
        }
    }
    wub_sync_dma(0, WUB_DMA_SIZE);
    printf("WAFFLE USBBLK: composite bulk block armed; DMA=%llx size=%u eps=%u/%u\n",
           wub_bus.pa, WUB_DMA_SIZE, num_eps, num_in);
}

static void wub_transfer(unsigned ep, unsigned buf, unsigned length)
{
    if (u.failed || (ep != 6 && ep != 7) || u.busy[ep] ||
        buf > WUB_DMA_SIZE || length > WUB_DMA_SIZE - buf) {
        fail("usbblk transfer bounds");
        return;
    }
    unsigned off = (ep == 6) ? WUB_TRB_OUT_OFF : WUB_TRB_IN_OFF;
    uint64_t p = wub_bus.pa + buf, t = wub_bus.pa + off;
    volatile uint32_t *trb = (volatile uint32_t *)(wub_bus.dma + off);
    trb[0] = (uint32_t)p;
    trb[1] = (uint32_t)(p >> 32);
    trb[2] = length;
    trb[3] = T_NORMAL | TRB_FLAGS;
    u.requested[ep] = length;
    if (length) wub_sync_dma(buf, length);
    wub_sync_dma(off, 16);
    u.busy[ep] = true;
    (void)epcmd(ep, START, (uint32_t)(t >> 32), (uint32_t)t, 0);
}

static int wub_complete_ep(unsigned ep)
{
    if ((ep != 6 && ep != 7) || !u.busy[ep]) { fail("usbblk unexpected completion"); return -1; }
    unsigned off = (ep == 6) ? WUB_TRB_OUT_OFF : WUB_TRB_IN_OFF;
    wub_sync_dma(off, 16);
    volatile uint32_t *t = (volatile uint32_t *)(wub_bus.dma + off);
    unsigned left = t[2] & 0xffffffu;
    if ((t[3] & HWO) || (t[2] >> 28) || left > u.requested[ep]) {
        fail("usbblk invalid DMA completion");
        return -1;
    }
    u.busy[ep] = false;
    return (int)(u.requested[ep] - left);
}

static void wub_finish_locked(int err)
{
    wub_bus.state = WUB_IDLE;
    wub_bus.err = err;
    wub_bus.done = 1;
    if (err) wub_bus.io_err++;
    else wub_bus.io_ok++;
    thread_wakeup(&wub_bus.wait_chan);
}

static void wub_abort(void)
{
    lck_mtx_lock(&wub_lck);
    wub_bus.ep_ready = 0;
    if (wub_bus.state != WUB_IDLE) wub_finish_locked(EIO);
    lck_mtx_unlock(&wub_lck);
}

static int wub_configure_eps(void)
{
    if (!waffle_usbblk_enabled() || !wub_bus.dma) return 0;
    if (epcmd(6, SET_CONFIG, (2u << 1) | (u.mps << 3), (1u << 8) | (6u << 25), 0) ||
        epcmd(6, SET_RESOURCE, 1, 0, 0) ||
        epcmd(7, SET_CONFIG, (2u << 1) | (u.mps << 3) | (3u << 17), (1u << 8) | (7u << 25), 0) ||
        epcmd(7, SET_RESOURCE, 1, 0, 0))
        return -1;
    wr(DALEPENA, 3u | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7));
    lck_mtx_lock(&wub_lck);
    wub_bus.ep_ready = 1;
    lck_mtx_unlock(&wub_lck);
    printf("WAFFLE USBBLK: EP3 OUT/IN configured (MPS %u); awaiting host daemon\n", u.mps);
    return 0;
}

static void wub_on_complete(unsigned ep, unsigned status)
{
    int n = wub_complete_ep(ep);
    if (n < 0) { wub_abort(); return; }
    if (status & 1) { fail("usbblk bus error"); wub_abort(); return; }
    lck_mtx_lock(&wub_lck);
    if (wub_bus.state == WUB_IDLE) { lck_mtx_unlock(&wub_lck); return; }

    if (wub_bus.state == WUB_SEND_REQ && ep == 7) {
        wub_bus.tx_bytes += (unsigned)n;
        if (wub_bus.req.op == WUB_OP_WRITE) {
            unsigned bytes = wub_bus.req.count * WUB_BLOCK_BYTES;
            wub_bus.state = WUB_SEND_DATA;
            wub_transfer(7, WUB_DATA_OFF, bytes);
        } else {
            wub_bus.state = WUB_RECV_RSP;
            wub_transfer(6, WUB_HDR_OUT_OFF, u.mps);
        }
    } else if (wub_bus.state == WUB_SEND_DATA && ep == 7) {
        wub_bus.tx_bytes += (unsigned)n;
        wub_bus.state = WUB_RECV_RSP;
        wub_transfer(6, WUB_HDR_OUT_OFF, u.mps);
    } else if (wub_bus.state == WUB_RECV_RSP && ep == 6) {
        wub_bus.rx_bytes += (unsigned)n;
        wub_sync_dma(WUB_HDR_OUT_OFF, WUB_HDR_BYTES);
        if (n != (int)WUB_HDR_BYTES ||
            !wub_decode_rsp((const uint8_t *)(wub_bus.dma + WUB_HDR_OUT_OFF), &wub_bus.rsp) ||
            wub_bus.rsp.seq != wub_bus.req.seq) {
            wub_finish_locked(EIO);
        } else if (wub_bus.req.op == WUB_OP_READ && wub_bus.rsp.status == 0) {
            unsigned bytes = wub_bus.req.count * WUB_BLOCK_BYTES;
            if (wub_bus.rsp.block_size != bytes) {
                wub_finish_locked(EIO);
            } else {
                wub_bus.state = WUB_RECV_DATA;
                wub_transfer(6, WUB_DATA_OFF, bytes);
            }
        } else {
            wub_finish_locked(0);
        }
    } else if (wub_bus.state == WUB_RECV_DATA && ep == 6) {
        unsigned bytes = wub_bus.req.count * WUB_BLOCK_BYTES;
        wub_bus.rx_bytes += (unsigned)n;
        wub_sync_dma(WUB_DATA_OFF, bytes);
        if (n != (int)bytes ||
            wub_crc32((const void *)(wub_bus.dma + WUB_DATA_OFF), bytes) != wub_bus.rsp.data_crc) {
            wub_finish_locked(EIO);
        } else {
            memcpy(wub_bus.caller_buf, (const void *)(wub_bus.dma + WUB_DATA_OFF), bytes);
            wub_finish_locked(0);
        }
    } else {
        wub_finish_locked(EIO);
    }
    lck_mtx_unlock(&wub_lck);
}

static int wub_poll(void)
{
    int active = 0;
    lck_mtx_lock(&wub_lck);
    if (wub_bus.state == WUB_QUEUED) {
        if (!u.configured || u.failed || !wub_bus.ep_ready) {
            if (wub_now_ns() >= wub_bus.deadline_ns) wub_finish_locked(ETIMEDOUT);
        } else if (!u.busy[6] && !u.busy[7]) {
            uint8_t *hdr = (uint8_t *)(wub_bus.dma + WUB_HDR_IN_OFF);
            if (wub_bus.req.op == WUB_OP_WRITE) {
                unsigned bytes = wub_bus.req.count * WUB_BLOCK_BYTES;
                memcpy((void *)(wub_bus.dma + WUB_DATA_OFF), wub_bus.caller_buf, bytes);
            }
            if (!wub_encode_req(hdr, &wub_bus.req)) {
                wub_finish_locked(EINVAL);
            } else {
                wub_bus.state = WUB_SEND_REQ;
                wub_transfer(7, WUB_HDR_IN_OFF, WUB_HDR_BYTES);
                active = 1;
            }
        }
    } else if (wub_bus.state != WUB_IDLE) {
        active = 1;
        if (u.failed || !u.configured) {
            wub_finish_locked(EIO);
            active = 0;
        } else if (wub_now_ns() >= wub_bus.deadline_ns) {
            printf("WAFFLE USBBLK: request timeout op=%u seq=%u state=%d\n",
                   wub_bus.req.op, wub_bus.req.seq, (int)wub_bus.state);
            /* Cancel any stuck EP transfer before releasing caller. */
            for (unsigned ep = 6; ep <= 7; ep++) {
                if (u.busy[ep] && u.resource[ep]) {
                    (void)epcmd(ep, END | FORCE_REMOVE | (u.resource[ep] << 16), 0, 0, 0);
                    u.busy[ep] = false;
                }
            }
            wub_finish_locked(ETIMEDOUT);
            active = 0;
        }
    }
    lck_mtx_unlock(&wub_lck);
    return active;
}

int waffle_usbblk_xfer(uint32_t op, uint64_t lba, uint32_t count, void *buf,
                       uint32_t *out_status, uint64_t *out_blocks, uint32_t *out_flags)
{
    if (!waffle_usbblk_enabled() || !wub_bus.dma) return ENXIO;
    if ((op == WUB_OP_READ || op == WUB_OP_WRITE) && (!buf || !count || count > WUB_MAX_BLOCKS))
        return EINVAL;

    /* Serialize callers so response/error fields cannot be clobbered before readback. */
    lck_mtx_lock(&wub_caller_lck);
    lck_mtx_lock(&wub_lck);
    wub_bus.seq++;
    if (!wub_bus.seq) wub_bus.seq = 1;
    wub_bus.req.op = op;
    wub_bus.req.seq = wub_bus.seq;
    wub_bus.req.count = count;
    wub_bus.req.lba = lba;
    wub_bus.req.data_crc = (op == WUB_OP_WRITE) ? wub_crc32(buf, (size_t)count * WUB_BLOCK_BYTES) : 0u;
    wub_bus.caller_buf = buf;
    wub_bus.done = 0;
    wub_bus.err = 0;
    uint64_t timeout_ms = (op == WUB_OP_INFO) ? 5000ULL : 10000ULL;
    wub_bus.deadline_ns = wub_now_ns() + timeout_ms * 1000000ULL;
    wub_bus.state = WUB_QUEUED;
    thread_wakeup(&u.sleep_channel);

    while (!wub_bus.done) {
        (void)lck_mtx_sleep(&wub_lck, LCK_SLEEP_DEFAULT, &wub_bus.wait_chan, THREAD_UNINT);
    }
    int err = wub_bus.err;
    if (!err) {
        if (out_status) *out_status = wub_bus.rsp.status;
        if (out_blocks) *out_blocks = wub_bus.rsp.total_blocks;
        if (out_flags) *out_flags = wub_bus.rsp.data_crc;
    }
    lck_mtx_unlock(&wub_lck);
    lck_mtx_unlock(&wub_caller_lck);
    return err;
}
