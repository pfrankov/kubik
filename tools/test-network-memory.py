#!/usr/bin/env python3
"""Prove actual pinned lwIP pbuf sizing for bounded duplex voice traffic."""
import os
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
idf = Path(os.environ['IDF_PATH'])
source = (idf / 'components/lwip/lwip/src/core/tcp_out.c').read_text()
start = source.index('#if TCP_OVERSIZE\nstatic struct pbuf *\ntcp_pbuf_prealloc')
end = source.index('#endif /* TCP_OVERSIZE */', start) + len('#endif /* TCP_OVERSIZE */')
actual = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint16_t u16_t;
typedef uint8_t u8_t;
typedef int pbuf_layer;
struct tcp_pcb { unsigned flags; void *unsent, *unacked; };
struct pbuf { u16_t len, tot_len; void *next; };
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define TCP_OVERSIZE 1440
#define TF_NODELAY 1
#define TCP_WRITE_FLAG_MORE 2
#define LWIP_MIN(a,b) ((a)<(b)?(a):(b))
#define LWIP_MEM_ALIGN_SIZE(n) (((n)+3)&~3)
#define TCP_OVERSIZE_CALC_LENGTH(n) ((n)+TCP_OVERSIZE)
/* HOOK */
#define LWIP_ASSERT(message, condition) assert(condition)
#define LWIP_UNUSED_ARG(value) (void)(value)
#define PBUF_RAM 0
static unsigned allocated;
static struct pbuf *pbuf_alloc(pbuf_layer layer,u16_t len,int kind) {
    (void)layer; (void)kind; allocated=len;
    static struct pbuf p; p.len=p.tot_len=len; p.next=NULL; return &p;
}
/* ACTUAL */
int main(void) {
    const unsigned sizes[]={69,1005,1275,1440};
    for(unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
        u16_t oversize=0; struct tcp_pcb pcb={.flags=TF_NODELAY};
        struct pbuf *p=tcp_pbuf_prealloc(0,sizes[i],1440,&oversize,&pcb,0,1);
        assert(p->len==sizes[i]);
        assert(allocated==(LWIP_NETIF_TX_SINGLE_PBUF ? 1440 : sizes[i]));
        assert(oversize==(LWIP_NETIF_TX_SINGLE_PBUF ? 1440-sizes[i] : 0));
    }
    // Ordinary Nagle/MORE writes retain the SDK's padding policy.
    u16_t spare=0; struct tcp_pcb nagle={0};
    assert(tcp_pbuf_prealloc(0,69,1440,&spare,&nagle,TCP_WRITE_FLAG_MORE,1)->len==69);
    assert(allocated==1440 && spare==1371);
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-tcp-memory-') as tmp:
    src, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    hook = (root / 'firmware/main/lwip_hooks.h').read_text().replace('#pragma once', '')
    for prefix in ('', '#define KUBIK_TCP_OUT_ALLOCATIONS 1\n'):
        src.write_text(fixture.replace('/* ACTUAL */', actual).replace('/* HOOK */', prefix + hook))
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print('lwIP: real allocator body preserves exact payload size without full-MSS padding; prior oversized profile reproduced')

commands = json.loads((root / 'firmware/build/compile_commands.json').read_text())
marked = [Path(item['file']).name for item in commands if 'KUBIK_TCP_OUT_ALLOCATIONS=1' in item['command']]
assert marked == ['tcp_out.c'], f'TCP allocation override escaped its source: {marked}'
