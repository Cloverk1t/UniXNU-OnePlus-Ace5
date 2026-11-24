/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Host unit tests for zero-write USB Bulk host-backed block protocol,
 * composite USB descriptors, HFSX root validator, and I/O bounds.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/storage/giulia_storage_usbblk_proto.h"

int main(void)
{
    /* Verify composite USB configuration descriptor framing and total length. */
    assert(sizeof(wub_device_desc) == 18 && wub_device_desc[4] == 0xef &&
           wub_device_desc[5] == 0x02 && wub_device_desc[6] == 0x01);
    assert(sizeof(wub_config_desc) == 98);
    assert(wub_config_desc[2] == 98 && wub_config_desc[3] == 0 && wub_config_desc[4] == 3);
    unsigned pos = 0, if_count = 0, bulk_eps = 0, saw_iad = 0, saw_vendor = 0;
    while (pos < sizeof(wub_config_desc)) {
        uint8_t len = wub_config_desc[pos];
        uint8_t type = wub_config_desc[pos + 1];
        assert(len >= 2 && pos + len <= sizeof(wub_config_desc));
        if (type == 0x0b) saw_iad++;
        if (type == 4) {
            if_count++;
            if (wub_config_desc[pos + 2] == WUB_IF_NUMBER &&
                wub_config_desc[pos + 5] == WUB_IF_CLASS &&
                wub_config_desc[pos + 6] == WUB_IF_SUBCLASS &&
                wub_config_desc[pos + 7] == WUB_IF_PROTOCOL)
                saw_vendor++;
        }
        if (type == 5 && (wub_config_desc[pos + 3] & 3) == 2) bulk_eps++;
        pos += len;
    }
    assert(pos == sizeof(wub_config_desc));
    assert(saw_iad == 1 && if_count == 3 && saw_vendor == 1 && bulk_eps == 4);

    /* Verify IEEE CRC32 test vector. */
    assert(wub_crc32("123456789", 9) == 0xcbf43926u);

    /* Request encode/decode round-trip and bit-flip rejection. */
    uint8_t wire[WUB_HDR_BYTES];
    struct wub_req req = {.op = WUB_OP_READ, .seq = 42, .count = 8, .lba = 0x123456789ULL, .data_crc = 0};
    struct wub_req out_req = {0};
    assert(wub_encode_req(wire, &req));
    assert(wub_decode_req(wire, &out_req));
    assert(out_req.op == req.op && out_req.seq == req.seq &&
           out_req.count == req.count && out_req.lba == req.lba && out_req.data_crc == 0);
    for (unsigned i = 0; i < WUB_HDR_BYTES; i++) {
        wire[i] ^= 1u;
        assert(!wub_decode_req(wire, &out_req));
        wire[i] ^= 1u;
    }

    /* Reject out-of-range block counts and malformed INFO/READ/WRITE headers. */
    struct wub_req bad = req;
    bad.count = 0; assert(!wub_encode_req(wire, &bad));
    bad.count = WUB_MAX_BLOCKS + 1; assert(!wub_encode_req(wire, &bad));
    bad.count = 1; bad.data_crc = 1; assert(!wub_encode_req(wire, &bad)); /* READ must have data_crc=0 */
    bad.op = WUB_OP_INFO; bad.count = 1; bad.data_crc = 0; assert(!wub_encode_req(wire, &bad));
    bad.count = 0; bad.lba = 1; assert(!wub_encode_req(wire, &bad));
    bad.lba = 0; assert(wub_encode_req(wire, &bad));
    bad.op = WUB_OP_WRITE; bad.count = 4; bad.lba = 10; bad.data_crc = 0xdeadbeefu;
    assert(wub_encode_req(wire, &bad));
    assert(wub_decode_req(wire, &out_req) && out_req.data_crc == 0xdeadbeefu);

    /* Response encode/decode round-trip and corruption rejection. */
    struct wub_rsp rsp = {.status = 0, .seq = 42, .block_size = 4096, .total_blocks = 8388608ULL, .data_crc = WUB_FLAG_WRITABLE};
    struct wub_rsp out_rsp = {0};
    assert(wub_encode_rsp(wire, &rsp));
    assert(wub_decode_rsp(wire, &out_rsp));
    assert(out_rsp.status == 0 && out_rsp.seq == 42 && out_rsp.block_size == 4096 &&
           out_rsp.total_blocks == 8388608ULL && out_rsp.data_crc == WUB_FLAG_WRITABLE);
    wire[12] ^= 0x80;
    assert(!wub_decode_rsp(wire, &out_rsp));

    /* I/O bounds check against negative/overflow sectors. */
    uint64_t off = 0;
    assert(wub_io_bounds(0, 512, 4096, 16ULL * 1024 * 1024 * 1024, &off) == 0 && off == 0);
    assert(wub_io_bounds(-1, 512, 512, 4096, &off) != 0);
    assert(wub_io_bounds(8, 512, 512, 4096, &off) != 0);
    assert(wub_io_bounds(7, 512, 513, 4096, &off) != 0);
    assert(wub_io_bounds(7, 512, 512, 4096, &off) == 0 && off == 3584);

    /* Generalized HFSX root validator (both 16 GiB and 32 GiB). */
    uint8_t blk0[WUB_BLOCK_BYTES];
    memset(blk0, 0, sizeof(blk0));
    uint8_t *h = blk0 + 1024;
    h[0] = 'H'; h[1] = 'X'; h[2] = 0; h[3] = 5;
    h[4] = 0; h[5] = 0; h[6] = 0x20; h[7] = 0; /* journaled */
    h[15] = 2; /* journal info block */
    h[42] = 0x10; /* 4096 block size */
    /* 32 GiB = 8388608 = 0x00800000 blocks */
    h[45] = 0x80;
    /* Finder UUID = 0x34320965bfe52d66 */
    uint8_t uuid_bytes[8] = {0x34, 0x32, 0x09, 0x65, 0xbf, 0xe5, 0x2d, 0x66};
    memcpy(h + 104, uuid_bytes, 8);
    assert(wub_hfs_root(blk0, 8388608ULL, 0x34320965bfe52d66ULL));
    assert(wub_hfs_root(blk0, 8388608ULL, 0));
    assert(!wub_hfs_root(blk0, 4194304ULL, 0x34320965bfe52d66ULL));
    assert(!wub_hfs_root(blk0, 8388608ULL, 0x1111111111111111ULL));
    h[6] = 0; /* non-journaled must be rejected */
    assert(!wub_hfs_root(blk0, 8388608ULL, 0x34320965bfe52d66ULL));

    puts("USB Bulk block protocol: composite descriptors, CRC32 framing, bounds and HFSX root validation passed");
    return 0;
}
