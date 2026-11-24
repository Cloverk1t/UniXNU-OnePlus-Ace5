/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Zero-write USB Bulk host-backed block protocol and composite USB descriptors.
 * Shared between the XNU kernel (DWC3 device), macOS IOKit host daemon, and
 * host-side ASan/UBSan tests. All wire fields are explicit little-endian.
 */
#ifndef GIULIA_STORAGE_USBBLK_PROTO_H
#define GIULIA_STORAGE_USBBLK_PROTO_H
#include <stdint.h>
#include <stddef.h>

#define WUB_REQ_MAGIC       0x31425557u /* "WUB1" */
#define WUB_RSP_MAGIC       0x31525557u /* "WUR1" */
#define WUB_HDR_BYTES       32u
#define WUB_BLOCK_BYTES     4096u
#define WUB_MAX_BLOCKS      8u
#define WUB_MAX_DATA_BYTES  (WUB_BLOCK_BYTES * WUB_MAX_BLOCKS) /* 32 KiB */

#define WUB_OP_INFO         1u
#define WUB_OP_READ         2u
#define WUB_OP_WRITE        3u
#define WUB_OP_SYNC         4u

#define WUB_FLAG_WRITABLE   1u

#define WUB_VID             0x1209u
#define WUB_PID             0x0001u
#define WUB_IF_NUMBER       2u
#define WUB_IF_CLASS        0xffu
#define WUB_IF_SUBCLASS     0x57u /* 'W' */
#define WUB_IF_PROTOCOL     0x42u /* 'B' */
#define WUB_EP_OUT_ADDR     0x03u /* DWC3 physical EP 6 */
#define WUB_EP_IN_ADDR      0x83u /* DWC3 physical EP 7 */

/* Composite device descriptor (IAD): IF0+IF1 = CDC ACM, IF2 = Vendor Bulk Block. */
static const uint8_t wub_device_desc[18] = {
    18, 1, 0x00, 0x02, 0xef, 0x02, 0x01, 64,
    0x09, 0x12, 0x01, 0x00, 0x00, 0x04, 1, 2, 3, 1
};

static const uint8_t wub_qualifier_desc[10] = {
    10, 6, 0x00, 0x02, 0xef, 0x02, 0x01, 64, 1, 0
};

/* 9 (cfg) + 8 (IAD) + 58 (CDC IF0+IF1) + 23 (Vendor IF2 + EP3 OUT/IN) = 98 bytes. */
static const uint8_t wub_config_desc[98] = {
    /* Configuration descriptor */
    9, 2, 98, 0, 3, 1, 0, 0x80, 250,
    /* Interface Association Descriptor for CDC ACM (IF0 + IF1) */
    8, 0x0b, 0, 2, 2, 2, 1, 0,
    /* Interface 0: CDC Control */
    9, 4, 0, 0, 1, 2, 2, 1, 0,
    5, 0x24, 0, 0x10, 1,
    5, 0x24, 1, 0, 1,
    4, 0x24, 2, 2,
    5, 0x24, 6, 0, 1,
    7, 5, 0x82, 3, 16, 0, 9,
    /* Interface 1: CDC Data */
    9, 4, 1, 0, 2, 0x0a, 0, 0, 0,
    7, 5, 0x01, 2, 0, 2, 0,
    7, 5, 0x81, 2, 0, 2, 0,
    /* Interface 2: Vendor-specific Bulk Block (waffle_usbblk) */
    9, 4, WUB_IF_NUMBER, 0, 2, WUB_IF_CLASS, WUB_IF_SUBCLASS, WUB_IF_PROTOCOL, 0,
    7, 5, WUB_EP_OUT_ADDR, 2, 0, 2, 0,
    7, 5, WUB_EP_IN_ADDR, 2, 0, 2, 0
};

struct wub_req {
    uint32_t op;
    uint32_t seq;
    uint32_t count;
    uint64_t lba;
    uint32_t data_crc;
};

struct wub_rsp {
    uint32_t status;
    uint32_t seq;
    uint32_t block_size;
    uint64_t total_blocks;
    uint32_t data_crc;
};

static inline uint32_t wub_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t wub_le64(const uint8_t *p)
{
    return (uint64_t)wub_le32(p) | ((uint64_t)wub_le32(p + 4) << 32);
}

static inline uint32_t wub_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void wub_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void wub_put64(uint8_t *p, uint64_t v)
{
    wub_put32(p, (uint32_t)v);
    wub_put32(p + 4, (uint32_t)(v >> 32));
}

static inline uint32_t wub_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline int wub_req_valid(const struct wub_req *r)
{
    if (!r) return 0;
    if (r->op == WUB_OP_INFO || r->op == WUB_OP_SYNC)
        return r->count == 0 && r->lba == 0 && r->data_crc == 0;
    if (r->op == WUB_OP_READ)
        return r->count >= 1 && r->count <= WUB_MAX_BLOCKS && r->data_crc == 0;
    if (r->op == WUB_OP_WRITE)
        return r->count >= 1 && r->count <= WUB_MAX_BLOCKS;
    return 0;
}

static inline int wub_encode_req(uint8_t out[WUB_HDR_BYTES], const struct wub_req *r)
{
    if (!out || !wub_req_valid(r)) return 0;
    wub_put32(out + 0, WUB_REQ_MAGIC);
    wub_put32(out + 4, r->op);
    wub_put32(out + 8, r->seq);
    wub_put32(out + 12, r->count);
    wub_put64(out + 16, r->lba);
    wub_put32(out + 24, r->data_crc);
    wub_put32(out + 28, wub_crc32(out, 28));
    return 1;
}

static inline int wub_decode_req(const uint8_t in[WUB_HDR_BYTES], struct wub_req *r)
{
    if (!in || !r) return 0;
    if (wub_le32(in + 0) != WUB_REQ_MAGIC) return 0;
    if (wub_le32(in + 28) != wub_crc32(in, 28)) return 0;
    struct wub_req tmp;
    tmp.op = wub_le32(in + 4);
    tmp.seq = wub_le32(in + 8);
    tmp.count = wub_le32(in + 12);
    tmp.lba = wub_le64(in + 16);
    tmp.data_crc = wub_le32(in + 24);
    if (!wub_req_valid(&tmp)) return 0;
    *r = tmp;
    return 1;
}

static inline int wub_encode_rsp(uint8_t out[WUB_HDR_BYTES], const struct wub_rsp *r)
{
    if (!out || !r) return 0;
    wub_put32(out + 0, WUB_RSP_MAGIC);
    wub_put32(out + 4, r->status);
    wub_put32(out + 8, r->seq);
    wub_put32(out + 12, r->block_size);
    wub_put64(out + 16, r->total_blocks);
    wub_put32(out + 24, r->data_crc);
    wub_put32(out + 28, wub_crc32(out, 28));
    return 1;
}

static inline int wub_decode_rsp(const uint8_t in[WUB_HDR_BYTES], struct wub_rsp *r)
{
    if (!in || !r) return 0;
    if (wub_le32(in + 0) != WUB_RSP_MAGIC) return 0;
    if (wub_le32(in + 28) != wub_crc32(in, 28)) return 0;
    r->status = wub_le32(in + 4);
    r->seq = wub_le32(in + 8);
    r->block_size = wub_le32(in + 12);
    r->total_blocks = wub_le64(in + 16);
    r->data_crc = wub_le32(in + 24);
    return 1;
}

static inline int wub_io_bounds(int64_t sector, uint32_t secsize, uint32_t count,
                                uint64_t total_bytes, uint64_t *off)
{
    if (!off || sector < 0 || !secsize || (uint64_t)sector > total_bytes / secsize) return 1;
    *off = (uint64_t)sector * secsize;
    return count > total_bytes - *off;
}

/* Validate an unpartitioned journaled HFSX volume header at block 0 (offset 1024).
 * Accepts 16 GiB, 32 GiB, or any host-exported 4 KiB-block HFSX volume matching
 * expected_blocks; when expected_uuid is nonzero, requires exact Finder UUID.
 */
static inline int wub_hfs_root(const uint8_t *block0, uint64_t expected_blocks,
                               uint64_t expected_uuid)
{
    if (!block0 || !expected_blocks || expected_blocks > 0xffffffffULL) return 0;
    const uint8_t *h = block0 + 1024;
    uint32_t attrs = wub_be32(h + 4);
    uint32_t journal = wub_be32(h + 12);
    uint32_t bsize = wub_be32(h + 40);
    uint32_t total = wub_be32(h + 44);
    uint64_t id = ((uint64_t)wub_be32(h + 104) << 32) | wub_be32(h + 108);
    if (h[0] != 'H' || h[1] != 'X' || h[2] != 0 || h[3] != 5) return 0;
    if (!(attrs & 0x2000u) || bsize != WUB_BLOCK_BYTES) return 0;
    if ((uint64_t)total != expected_blocks) return 0;
    if (!journal || journal >= total) return 0;
    if (expected_uuid && id != expected_uuid) return 0;
    return 1;
}

#endif
