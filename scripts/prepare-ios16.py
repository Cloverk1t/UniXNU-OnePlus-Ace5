"""Generate Ace 5 patches from the pinned Vermeer integration, not Redmi binaries."""
from ios16_sources import source_path
import argparse
import shutil
import subprocess
import sys
import runpy
import re
import json
import difflib
from pathlib import Path
from ios16_display import native_display_patch
from ios16_terminal import terminal_sources, debug_trust_source

def edit(path, old, new, count=1):
    text = path.read_text(errors='surrogateescape')
    if text.count(old) != count:
        raise RuntimeError(f'Source drift: {path.name}: {old!r}: {text.count(old)} != {count}')
    path.write_text(text.replace(old, new), errors='surrogateescape')

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--gui', action='store_true', help='Include bounded display/surface/HID compatibility services')
    p.add_argument('--touch', action='store_true', help='Include opt-in Ace 5 TouchCom v1 hardware bring-up')
    p.add_argument('--wifi', action='store_true', help='Include WCN7850 PCIe/MHI/QMI/WMI scan, single-station open/WPA2-CCMP transport and iOS16 IO80211 service')
    p.add_argument('--gpu', action='store_true', help='Include opt-in A750 firmware/host-memory preparation and gated power observation; not acceleration')
    p.add_argument('--audio', action='store_true', help='Include ADSP/GLINK/SIPA coordinator; native platform/PCM provider still required')
    p.add_argument('--modem', action='store_true', help='Include unified modem lifecycle and shared resource broker (GUI profile)')
    p.add_argument('--debug-userland', type=Path, help='Exact signed debug daemon/shell manifest from offline staging')
    a = p.parse_args()
    if a.touch and not a.gui:p.error('--touch requires --gui')
    if a.wifi and not a.gui:p.error('--wifi requires --gui')
    if a.gpu and not a.gui:p.error('--gpu requires --gui')
    if a.audio and not a.gui:p.error('--audio requires --gui')
    if a.modem and not a.gui:p.error('--modem requires --gui')
    if a.debug_userland and not a.gui:p.error('--debug-userland requires --gui')
    debug_trust=debug_trust_source(a.debug_userland) if a.debug_userland else None
    revision = subprocess.check_output(['git', '-C', str(a.reference), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != 'd68e05df683f863f054b0f9c063c54d2b1342b2a':
        raise RuntimeError('Unpinned reference checkout')
    shutil.copytree(a.reference / 'xnu', a.work)
    v = a.work / 'vermeer'
    usb, probe, block = [v / f'vermeer_{n}.c' for n in ['usb_debug','ufs_probe','ufs_block']]
    # Allocator-owned physically contiguous pages, bounded by the boot memory
    # actually passed to XNU. Never infer an owned range from the K70 addresses.
    for name in ['vermeer_ufs_dma.h','vermeer_usb_protocol.h']:
        path = v / name
        edit(path, '#include <sys/types.h>', '#include <sys/types.h>\nextern unsigned long gPhysBase, gPhysSize;')
    edit(v/'vermeer_ufs_dma.h', 'if (pa >= 0xc0000000ULL && pa <= 0xd0000000ULL - VU_DMA_SIZE) return 1;',
         'if (pa < gPhysBase || gPhysSize < VU_DMA_SIZE || pa - gPhysBase > gPhysSize - VU_DMA_SIZE) return 0;\n    if (pa <= 0x100000000ULL - VU_DMA_SIZE) return 1;')
    edit(v/'vermeer_ufs_dma.h', 'pa >= 0x8c0000000ULL && pa <= 0x940000000ULL - VU_DMA_SIZE',
         'pa <= (1ULL << 36) - VU_DMA_SIZE')
    edit(v/'vermeer_usb_protocol.h', 'pa<0x8c0000000ULL ||\n        pa>=0x940000000ULL || size>0x940000000ULL-pa',
         'pa<gPhysBase ||\n        gPhysSize<size || pa-gPhysBase>gPhysSize-size')
    for path in [usb, probe]:
        edit(path, '0x4c017e69', '0x4c017e6b')
        edit(path, '0x60000046', '0x60000047')
    edit(usb, 'i<105', 'i<107')
    edit(usb, 'cb>=70', 'cb>=71')
    edit(usb, '0xd0000', '0x100000')
    edit(usb, 'K70DBG004', 'ACE5DBG001', 4)
    edit(usb, 'Vermeer Lab', 'Giulia Lab')
    edit(usb, 'Vermeer XNU Debug', 'Ace 5 XNU Debug')
    # SCR0 is logged, not compared to an unobserved K70 value; stream and
    # context policy remains mandatory, including VA64 for high addresses.
    edit(usb, 'read_reg(s,0)!=0x00280406 || ', '')
    edit(usb, 'return vusb_route_identity(route,cbar,sctlr)!=0;',
         'return vusb_route_identity(route,cbar,sctlr)!=0 &&\n        (((route>>16)&3)==1 || (read_reg(s,0x1800+4*cb)&1));')
    edit(probe, '0x90000', '0x100000')
    edit(probe, '61842431U', '123786239U')
    edit(probe, '0x45fbee46U', '0xb9fdad28U')
    # Embed a C-compatible guarded context handoff; the generated source patch
    # then carries the entire implementation with correct hunk lengths.
    guard = (Path(__file__).resolve().parents[1]/'ios16/storage/giulia_smmu.h').read_text()
    guard = guard.replace('#include <stdint.h>', '')
    edit(probe, 'static int identity_candidate(void)', guard + '\nstatic uint32_t gs_r(unsigned o) { return rd(SMMU+o); }\n'
         'static void gs_w(unsigned o,uint32_t v) { *(volatile uint32_t *)(SMMU+o)=v; __asm__ volatile("dsb sy" ::: "memory"); }\n'
         'static int giulia_take(void) { return gs_take(gs_r,gs_w,!rd(UFS+0x60)&&!rd(UFS+0x58)&&!rd(UFS+0x78)); }\n'
         'static int giulia_restore(void) { return gs_restore(gs_r,gs_w,!rd(UFS+0x60)&&!rd(UFS+0x58)&&!rd(UFS+0x78)); }\n'
         'static int identity_candidate(void)')
    text=probe.read_text(); start=text.index('    if (rd(SMMU + 0x20)',text.index('static int identity_candidate'))
    end=text.index('    /* Static buffer range',start)
    text=text[:start]+'    if (gs_plan(gs_r)) return 1;\n'+text[end:]; probe.write_text(text)
    edit(probe,'    wr(0x50, vu_lo(dma_pa));','    if (giulia_take()) return ENXIO;\n    wr(0x50, vu_lo(dma_pa));')
    edit(probe,'            wr(0x50, oldlo);','            if (giulia_restore()) return ENXIO;\n            wr(0x50, oldlo);')
    edit(probe,'0x80058','0xc6058')
    edit(block,'    wr(0x50, vu_lo(dma_pa));','    if (giulia_take()) return ENXIO;\n    wr(0x50, vu_lo(dma_pa));')
    edit(block,'    wr(0x50, vb.oldlo);','    if (giulia_restore()) { vb.failed=true; return; }\n    wr(0x50, vb.oldlo);')
    edit(v/'vermeer_media_policy.h','2570304ULL','4293356ULL')
    edit(v/'vermeer_media_policy.h','59272123ULL','119492879ULL')
    edit(block,'g.first != 2570304 || g.blocks != 59272123','g.first != 4293356 || g.blocks != 119492879')
    # Start the scheduler-based CDC worker at root selection, BEFORE UFS I/O.
    # Also support zero-write USB Bulk host-backed root (rd=waffleusb0) and selftest (waffle_usbblk_test=1).
    hook=v/'0021-vermeer-ios-root-hook.patch'
    edit(hook,'+766,22','+766,57')
    edit(hook,'+\t\tint error = vermeer_ufs_ios_rootdev(root);',
         '+\t\textern void vermeer_usb_debug_start(void);\n+\t\tvermeer_usb_debug_start();\n'
         '+\t\tint usbblk_test = 0;\n'
         '+\t\tPE_parse_boot_argn("waffle_usbblk_test", &usbblk_test, sizeof(usbblk_test));\n'
         '+\t\tif (usbblk_test == 1) {\n'
         '+\t\t\textern int waffle_usbblk_selftest(void);\n'
         '+\t\t\tint test_err = waffle_usbblk_selftest();\n'
         '+\t\t\tIOLog("WAFFLE USBBLK: selftest result %d; USB console remains active\\n", test_err);\n'
         '+\t\t\tfor (;;) IOSleep(1000);\n+\t\t}\n'
         '+\t\tint giulia_probe = 0;\n+\t\tPE_parse_boot_argn("giulia_ufs_probe", &giulia_probe, sizeof(giulia_probe));\n'
         '+\t\tif (giulia_probe == 1) {\n+\t\t\textern int vermeer_ufs_probe(void);\n'
         '+\t\t\tint probe_error = vermeer_ufs_probe();\n'
         '+\t\t\tIOLog("GIULIA: UFS read probe result %d; USB console remains active\\n", probe_error);\n'
         '+\t\t\t/* Keep the scheduler alive for CDC enumeration after a failed read. */\n'
         '+\t\t\tfor (;;) IOSleep(1000);\n+\t\t}\n'
         '+\t\tint error = vermeer_ufs_ios_rootdev(root);')
    edit(hook,'+\t\tgoto iofrootx;\n+\t}\n+#endif',
         '+\t\tgoto iofrootx;\n+\t}\n'
         '+\tif (strcmp(rdBootVar, "waffleusb0") == 0) {\n'
         '+\t\textern void vermeer_usb_debug_start(void);\n'
         '+\t\textern int waffle_usbblk_ios_rootdev(dev_t *dev);\n'
         '+\t\tOSSafeReleaseNULL(matching);\n'
         '+\t\tIOLog("WAFFLE USBBLK: selecting zero-write USB host root (UFS untouched)\\n");\n'
         '+\t\tvermeer_usb_debug_start();\n'
         '+\t\tint error = waffle_usbblk_ios_rootdev(root);\n'
         '+\t\tif (error) {\n'
         '+\t\t\tIOLog("WAFFLE USBBLK: host root unavailable (%d); USB console remains active\\n", error);\n'
         '+\t\t\tfor (;;) IOSleep(1000);\n'
         '+\t\t}\n'
         '+\t\tstrlcpy(rootName, rdBootVar, rootNameSize);\n'
         '+\t\tIOLog("BSD root: %s, major %d, minor %d\\n", rootName, major(*root), minor(*root));\n'
         '+\t\t*oflags = 0;\n'
         '+\t\tgoto iofrootx;\n+\t}\n+#endif')
    if a.gui:
        edit(usb,'static void command_line(void)','extern int sweet_sample_request(void);\nstatic void command_line(void)')
        edit(usb,'    else if (!strcmp(u.command,"status"))',
             '    else if (!strcmp(u.command,"sample")) printf("GIULIA DEBUG: sample wake requested=%d\\n",sweet_sample_request());\n'
             '    else if (!strcmp(u.command,"status"))')
        edit(usb,'help/status/ping/replay','help/status/ping/replay/sample')
        edit(usb,'commands help status ping replay;','commands help status ping replay sample;')
    terminal_sources(usb, block, edit, wifi=a.wifi, gui=a.gui, gpu=a.gpu, audio=a.audio, modem=a.modem)
    # Attach zero-write USB Bulk host-backed block transport (opt-in via waffle_usbblk=1 or rd=waffleusb0).
    repo_root = Path(__file__).resolve().parents[1]
    usb_proto = v / 'vermeer_usb_protocol.h'
    edit(usb_proto, 'index<=1 && !value && !len) return VUSB_INTERFACE;',
         'index<=2 && !value && !len) return VUSB_INTERFACE;')
    edit(usb_proto, 'index<=1 && !value && len==1) return VUSB_GET_INTERFACE;',
         'index<=2 && !value && len==1) return VUSB_GET_INTERFACE;')
    edit(usb_proto, '(index==1 || index==0x81 || index==0x82)) return VUSB_CLEAR_HALT;',
         '(index==1 || index==0x81 || index==0x82 || index==3 || index==0x83)) return VUSB_CLEAR_HALT;')
    wub_proto_src = (repo_root / 'ios16/storage/giulia_storage_usbblk_proto.h').read_text()
    wub_proto_src = wub_proto_src.replace('#include <stdint.h>\n', '').replace('#include <stddef.h>\n', '')
    wub_dwc3_src = (repo_root / 'ios16/storage/giulia_storage_usbblk_dwc3.inc').read_text().replace(
        '/* GIULIA_USBBLK_PROTO is embedded here by the profile generator. */', wub_proto_src)
    wub_dev_src = (repo_root / 'ios16/storage/giulia_storage_usbblk_dev.inc').read_text().replace(
        '/* GIULIA_USBBLK_PROTO is embedded here by the profile generator. */', wub_proto_src)
    edit(usb, 'static const unsigned trb_off[6]={0x1000,0x1040,0x1080,0x10c0,0x1100,0x1140};',
         'static const unsigned trb_off[8]={0x1000,0x1040,0x1080,0x10c0,0x1100,0x1140,0,0};')
    edit(usb, 'unsigned resource[6], requested[6];', 'unsigned resource[8], requested[8];')
    edit(usb, 'bool busy[6], failed, configured, dtr, notify;', 'bool busy[8], failed, configured, dtr, notify;')
    edit(usb, 'if (u.failed || ep>=6) return -1;', 'if (u.failed || ep>=8) return -1;')
    edit(usb, 'for (unsigned ep=2;ep<6;ep++) {', 'for (unsigned ep=2;ep<8;ep++) {')
    edit(usb, 'static void stop_data(void)', wub_dwc3_src + '\nstatic void stop_data(void)')
    edit(usb, 'u.configured=false; u.dtr=false; u.notify=false;',
         'u.configured=false; u.dtr=false; u.notify=false;\n    wub_abort();')
    edit(usb, 'wr(DALEPENA,3|(1u<<2)|(1u<<3)|(1u<<5));',
         'wr(DALEPENA,3|(1u<<2)|(1u<<3)|(1u<<5));\n    if (wub_configure_eps()) return;')
    edit(usb, 'case 1: if (value&255) break;memcpy(data,vusb_device,sizeof(vusb_device));n=sizeof(vusb_device);break;',
         'case 1: if (value&255) break;'
         'if (waffle_usbblk_enabled()) { memcpy(data,wub_device_desc,sizeof(wub_device_desc));n=sizeof(wub_device_desc); }'
         'else { memcpy(data,vusb_device,sizeof(vusb_device));n=sizeof(vusb_device); }break;')
    edit(usb, 'memcpy(data,vusb_config,sizeof(vusb_config));n=sizeof(vusb_config);',
         'if (waffle_usbblk_enabled()) { memcpy(data,wub_config_desc,sizeof(wub_config_desc));n=sizeof(wub_config_desc); }'
         'else { memcpy(data,vusb_config,sizeof(vusb_config));n=sizeof(vusb_config); }')
    edit(usb, 'case 6:if (!(value&255)) { memcpy(data,vusb_qualifier,sizeof(vusb_qualifier));n=sizeof(vusb_qualifier); }break;',
         'case 6:if (!(value&255)) {'
         'if (waffle_usbblk_enabled()) { memcpy(data,wub_qualifier_desc,sizeof(wub_qualifier_desc));n=sizeof(wub_qualifier_desc); }'
         'else { memcpy(data,vusb_qualifier,sizeof(vusb_qualifier));n=sizeof(vusb_qualifier); } }break;')
    edit(usb, '    if (type==1) {\n        int n=complete(ep);',
         '    if (type==1) {\n        if (ep==6 || ep==7) { wub_on_complete(ep,status); return; }\n        int n=complete(ep);')
    edit(usb, '        transmit();\n        uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);',
         '        transmit();\n        int wub_active=wub_poll();\n'
         '        uint64_t deadline;clock_interval_to_deadline(wub_active?50:1000,NSEC_PER_USEC,&deadline);')
    edit(usb, '    gs_disconnect();gscreen_disconnect();\n    wr(DEVTEN,0);',
         '    wub_abort();gs_disconnect();gscreen_disconnect();\n    wr(DEVTEN,0);')
    edit(usb, '    vm_offset_t qs=ml_io_map(0xa6f8800,0x100);',
         '    wub_init_dma(width);\n    vm_offset_t qs=ml_io_map(0xa6f8800,0x100);')
    edit(block, '    *dev = makedev(vb.bmajor, 0); return 0;\n}\n#endif\n#endif',
         '    *dev = makedev(vb.bmajor, 0); return 0;\n}\n' + wub_dev_src + '\n#endif\n#endif')
    sys.argv = [str(a.reference/'tools/prepare_vermeer.py'),str(a.work/'patches'),str(a.out),'--ios-gui' if a.gui else '--ios']
    runpy.run_path(sys.argv[0], run_name='__main__')
    for path in a.out.glob('*.patch'):
        text=path.read_text(errors='surrogateescape')
        for old,new in [('0xb8000000','0xd5100000'),('3168','3136'),('3200u','3168u'),
                        ('arm,cortex-a510','qcom,kryo'),('VERMEER USB:','GIULIA USB:'),
                        ('xiaomi,sweet','oneplus,giulia'),
                        ('strlcpy(name, "sweet",','strlcpy(name, "giulia",'),
                        ('strlcpy(name, "Redmi Note 10 Pro",','strlcpy(name, "OnePlus 12",')]:
            text=text.replace(old,new)
        if a.gui:
            # Rewrite added, removed and context lines alike so the complete
            # display patch series still applies atomically. These standalone
            # decimal constants describe the reference panel/HID coordinates.
            text=re.sub(r'\b3200\b','3168',text)
            text=text.replace('1440x3200','1440x3168').replace('VERMEER FB:','GIULIA FB:')
            text=text.replace('y > 3199 ? 3199','y > 3167 ? 3167')
            text=text.replace('K70 framebuffer','OnePlus 12 framebuffer').replace('K70 panel','OnePlus 12 panel')
        path.write_text(text,errors='surrogateescape')
    # OnePlus 12 (waffle) 1440x3168x4 framebuffer (17.4 MiB) at 0xd5100000..0xd6267000
    # crosses the 32 MiB ARM64 16K-granule L2 block boundary at 0xd6000000.
    # Map TWO contiguous 32 MiB L2 blocks (0xd4000000..0xd7ffffff) in both start.s (TTBR0)
    # and arm_vm_init.c (TTBR1), and preserve the +0x01100000 in-block offset on relocate.
    early0 = a.out / '0010-sweet-early-framebuffer-debug.patch'
    edit(early0, '@@ -609,6 +628,23 @@ Lkernelcache_base_found:',
         '@@ -609,6 +628,27 @@ Lkernelcache_base_found:')
    edit(early0,
         '+\tMOV64\tx10, 0xd5100000\n'
         '+\tMOV64\tx11, (ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX)\n'
         '+\torr\t\tx11, x11, x10\n'
         '+\tand\t\tx12, x10, #(ARM_TT_L2_INDEX_MASK)\n'
         '+\tlsr\t\tx12, x12, #(ARM_TT_L2_SHIFT)\n'
         '+\tadd\t\tx13, x25, #(2 * PGBYTES)\n'
         '+\tstr\t\tx11, [x13, x12, lsl #TTE_SHIFT]',
         '+\tMOV64\tx10, 0xd4000000\n'
         '+\tMOV64\tx11, (ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX)\n'
         '+\torr\t\tx11, x11, x10\n'
         '+\tand\t\tx12, x10, #(ARM_TT_L2_INDEX_MASK)\n'
         '+\tlsr\t\tx12, x12, #(ARM_TT_L2_SHIFT)\n'
         '+\tadd\t\tx13, x25, #(2 * PGBYTES)\n'
         '+\tstr\t\tx11, [x13, x12, lsl #TTE_SHIFT]\n'
         '+\tmov\t\tx10, #(1 << ARM_TT_L2_SHIFT)\n'
         '+\tadd\t\tx11, x11, x10\n'
         '+\tadd\t\tx12, x12, #1\n'
         '+\tstr\t\tx11, [x13, x12, lsl #TTE_SHIFT]')
    early1 = a.out / '0012-sweet-keep-debug-console-after-ttbr0.patch'
    edit(early1,
         '+\tassert(va + ARM_TT_L2_SIZE <= gVirtBase);\n'
         '+\ttt_entry_t l1 = cpu_tte[(va & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT];\n'
         '+\tassert((l1 & (ARM_TTE_VALID | ARM_TTE_TYPE_MASK)) == (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE));\n'
         '+\ttt_entry_t *l2 = (tt_entry_t *)phystokv(l1 & ARM_TTE_TABLE_MASK);\n'
         '+\ttt_entry_t *slot = &l2[(va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT];\n'
         '+\tassert(*slot == ARM_TTE_EMPTY);\n'
         '+\t*slot = 0xd5100000ULL | ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK |\n'
         '+\t    ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) |\n'
         '+\t    ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) |\n'
         '+\t    ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX;',
         '+\tassert(va + 2 * ARM_TT_L2_SIZE <= gVirtBase);\n'
         '+\ttt_entry_t l1 = cpu_tte[(va & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT];\n'
         '+\tassert((l1 & (ARM_TTE_VALID | ARM_TTE_TYPE_MASK)) == (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE));\n'
         '+\ttt_entry_t *l2 = (tt_entry_t *)phystokv(l1 & ARM_TTE_TABLE_MASK);\n'
         '+\ttt_entry_t *slot = &l2[(va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT];\n'
         '+\tassert(slot[0] == ARM_TTE_EMPTY && slot[1] == ARM_TTE_EMPTY);\n'
         '+\tconst tt_entry_t attr = ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) |\n'
         '+\t    ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX;\n'
         '+\tslot[0] = 0xd4000000ULL | attr;\n'
         '+\tslot[1] = (0xd4000000ULL + ARM_TT_L2_SIZE) | attr;')
    edit(early1,
         '+\tsweet_early_relocate(SWEET_EARLY_FB_VA);',
         '+\tsweet_early_relocate(SWEET_EARLY_FB_VA + (0xd5100000ULL - 0xd4000000ULL));')
    # Shared ARM cache-attribute path, used by both Wi-Fi and display buffers.
    # Ace 5 RAM lives above 4 GiB: widen ppnum_t before the physical-page shift.
    cache_patch='0081-giulia-pmap-high-physical-cache-sync.patch'
    shutil.copyfile(Path(__file__).resolve().parents[1]/'ios16'/cache_patch,a.out/cache_patch)
    reboot_patch='0082-giulia-fastboot-reboot.patch'
    shutil.copyfile(Path(__file__).resolve().parents[1]/'ios16'/reboot_patch,a.out/reboot_patch)
    capture=(a.reference/'tools/capture-debug.ps1').read_text().replace('K70DBG004','ACE5DBG001')
    if a.gui:
        # Complete GUI logging consumes the service-image sampler even when
        # the optional physical touch driver is omitted.
        shutil.copyfile(Path(__file__).resolve().parents[1]/'ios16/0070-giulia-service-images.patch',
                        a.out/'0070-giulia-service-images.patch')
        shutil.copyfile(Path(__file__).resolve().parents[1]/'ios16/0071-giulia-on-demand-samples.patch',
                        a.out/'0071-giulia-on-demand-samples.patch')
        patch = ('--- a/iokit/conf/files.arm64\n+++ b/iokit/conf/files.arm64\n'
                 '@@ -13,3 +13,4 @@\n'
                 ' iokit/Kernel/arm/SweetEntitlements.cpp                           optional iokitcpp\n'
                 ' iokit/Kernel/arm/SweetKeyStore.cpp                               optional iokitcpp\n'
                 '+iokit/Kernel/arm/GiuliaAMFI.cpp                                  optional iokitcpp\n'
                 ' iokit/Kernel/arm/SweetWatchdog.cpp                               optional iokitcpp\n'
                 '--- a/iokit/KernelConfigTables.cpp\n+++ b/iokit/KernelConfigTables.cpp\n'
                 '@@ -51,3 +51,8 @@\n'
                 '     "     \'IOMatchCategory\' = AppleKeyStore;"\n'
                 '     "   },"\n'
                 '+    "   {"\n'
                 '+    "     \'IOClass\'         = AppleMobileFileIntegrity;"\n'
                 '+    "     \'IOProviderClass\' = IOResources;"\n'
                 '+    "     \'IOMatchCategory\' = AppleMobileFileIntegrity;"\n'
                 '+    "   },"\n'
                 '     "   {"\n')
        for name in ('GiuliaAMFI.cpp', 'giulia_trust_catalog.h'):
            data = (source_path(Path(__file__).resolve().parents[1], name)).read_text()
            if name=='GiuliaAMFI.cpp' and debug_trust:
                anchor='class AppleMobileFileIntegrity : public IOService {'
                expression='return gtc_contains(&view, hash, length) != 0;'
                if data.count(anchor)!=1 or data.count(expression)!=1:
                    raise ValueError('AMFI debug trust integration source drift')
                data=data.replace(anchor,debug_trust+'\n'+anchor).replace(expression,
                    'return giulia_debug_trusted(hash, length) || gtc_contains(&view, hash, length) != 0;')
            data = data.replace('#include <stdint.h>\n', '').replace('#include <stddef.h>\n', '')
            patch += ''.join(difflib.unified_diff([], data.splitlines(True),
                              fromfile='/dev/null', tofile='b/iokit/Kernel/arm/'+name))
        (a.out/'0072-giulia-read-only-trust-catalog.patch').write_text(patch)
        for name in ('0073-giulia-complete-boot-log.patch','0074-giulia-ios-pid-hibernate.patch'):
            shutil.copyfile(Path(__file__).resolve().parents[1]/'ios16'/name,a.out/name)
        native_display_patch(a.out, Path(__file__).resolve().parents[1])
        from ios16_display_power import display_power_patch
        display_power_patch(a.out, Path(__file__).resolve().parents[1])
        from ios16_modules import emit_input_modules
        emit_input_modules(a.out, Path(__file__).resolve().parents[1], touch=a.touch)
        from ios16_modules import emit_optional_modules
        selected = [name for name in ("wifi", "gpu", "audio", "modem") if getattr(a, name)]
        emit_optional_modules(a.out, Path(__file__).resolve().parents[1], selected)
        capture=capture.replace("[ValidateSet('help','status','ping','replay')]","[ValidateSet('help','status','ping','replay','sample')]")
    (a.out/'capture-debug.ps1').write_text(capture)
    for name in ('debug-terminal.cmd', 'debug_terminal.py', 'ios16_screen.py', 'ios16_screen_viewer.html', 'terminal-requirements.txt'):
        shutil.copyfile(Path(__file__).resolve().parents[1]/'scripts'/name, a.out/name)
    shutil.copytree(Path(__file__).resolve().parents[1]/'ios16/userland',a.out/'userland')
    shutil.copyfile(Path(__file__).resolve().parents[1]/'ios16/common/giulia_debug_abi.h',a.out/'giulia_debug_abi.h')
    if a.debug_userland:
        shutil.copyfile(a.debug_userland,a.out/'debug-userland.json')
    checker=(a.reference/'tools/check_ios_kernel.py').read_text().replace('K70DBG004','ACE5DBG001').replace('VERMEER USB:','GIULIA USB:')
    if a.gui:
        checker=checker.replace('VERMEER FB:','GIULIA FB:').replace('1440x3200','1440x3168')
        checker=checker.replace('logical canvas 1125x2436 centered in 1440x3168',
                                'native panel 1440x3168 full coverage; UIKit canvas 1125x2436')
    (a.out/'check-kernel.py').write_text(checker)
    # A Windows working copy can contain CRLF even when Git stores LF. Copied
    # profile patches/scripts must match the Linux/macOS source they apply to.
    # Preserve every other byte, including upstream non-UTF8 source comments.
    for generated in a.out.iterdir():
        if generated.suffix in ('.patch', '.sh'):
            raw=generated.read_bytes()
            if b'\r\n' in raw:generated.write_bytes(raw.replace(b'\r\n',b'\n'))
    (a.out/'profile.json').write_text(json.dumps({'device':'OnePlus 12 PJD110 (waffle)',
        'usb_terminal_protocol':2,
        'high_physical_cache_sync_fix':True,
        'debug_userland_trust_included':bool(debug_trust),
        'usb_terminal_host':'debug-terminal.cmd',
        'usb_terminal_hardware_tested':False,
        'screen_forwarding_included':a.gui,
        'screen_forwarding_protocol':'GSC1 RGB565_LE up to 1440x3168, bounded CRC chunks, configurable max_size/max_fps' if a.gui else None,
        'screen_remote_input_included':a.gui,
        'screen_remote_input_protocol':'normalized touch, USB HID keyboard/modifiers and consumer buttons, 3-second release lease' if a.gui else None,
        'profile':'gui' if a.gui else 'base','requires_local_gui_adt':a.gui,
        'physical_touch_driver_included':a.touch,'touch_protocol':'TouchCom v1 legacy/FIFO or GPIO SPI' if a.touch else None,
        'wifi_runtime_included':a.wifi,
        'gpu_runtime_included':a.gpu,
        'gpu_terminal_included':a.gpu,
        'audio_runtime_included':a.audio,
        'audio_platform_provider_included':False,
        'modem_runtime_included':a.modem,
        'modem_runtime_abi':1 if a.modem else None,
        'modem_platform_broker_included':a.modem,
        'platform_broker_included':bool(a.wifi or a.gpu or a.audio or a.modem),
        'modem_native_provider_included':False,
        'modem_scope':'unified lifecycle, subscription/bearer/media/storage state and platform arbitration; native provider capability is explicit' if a.modem else None,
        'audio_scope':'pinned ADSP pack, boot/IPC/SIPA coordinator; native power/ICC and PCM provider missing' if a.audio else None,
        'gpu_scope':'pinned firmware, GMU host-memory preparation, optional gated read-only power/SMMU observation; no GPU submission' if a.gpu else None,
        'wifi_scope':'PCIe, MHI, QRTR/QMI, CE/HTC/WMI passive scan, IO80211 single-station open/WPA2-CCMP and TCL/REO Ethernet; hardware networking unverified' if a.wifi else None,
        'read_only_trust_catalog_query':a.gui,
        'complete_boot_logging_available':a.gui,
        'native_panel':{'width':1440,'height':3168,'stride':5760,
                        'presentation':'full-panel','touch':'full-panel normalized',
                        'uikit_canvas':[1125,2436]} if a.gui else None,
        'hardware_tested':False},indent=2)+'\n')

if __name__=='__main__': main()
