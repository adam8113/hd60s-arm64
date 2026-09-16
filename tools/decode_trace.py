#!/usr/bin/env python3
"""
decode a usbmon pcap capture of the Elgato HD60 S.

Extracts every USB control transfer to/from 0fd9:0074 and prints it in the
same shape as our transport log, so the vendor's real sequence can be diffed
against what our driver does.

usage: decode_trace.py capture.pcap [--bus N] [--csv out.csv] [--seq out.txt]
"""
import sys, struct, argparse

# Linux usbmon header (DLT_USB_LINUX_MMAPPED = 220), 64 bytes
USBMON_HDR = 64

def parse_pcap(path):
    with open(path, 'rb') as f:
        gh = f.read(24)
        if len(gh) < 24: return
        magic = struct.unpack('<I', gh[:4])[0]
        if magic in (0xa1b2c3d4, 0xa1b23c4d): endian = '<'
        elif magic in (0xd4c3b2a1, 0x4d3cb2a1): endian = '>'
        else:
            print(f"not a pcap file (magic 0x{magic:08x})"); return
        nano = magic in (0xa1b23c4d, 0x4d3cb2a1)
        linktype = struct.unpack(endian+'I', gh[20:24])[0]
        if linktype not in (220, 189):
            print(f"warning: linktype {linktype}, expected 220 (usbmon mmapped)")
        while True:
            ph = f.read(16)
            if len(ph) < 16: break
            ts_s, ts_u, caplen, origlen = struct.unpack(endian+'IIII', ph)
            data = f.read(caplen)
            if len(data) < caplen: break
            yield ts_s + (ts_u/1e9 if nano else ts_u/1e6), data

def decode(pkt):
    """Return dict for a usbmon record, or None."""
    if len(pkt) < USBMON_HDR: return None
    (urb_id, ev_type, xfer_type, epnum, devnum, busnum, flag_setup, flag_data,
     ts_sec, ts_usec, status, length, len_cap) = struct.unpack('<QcBBBHccqiiII', pkt[:40])
    setup = pkt[40:48]
    payload = pkt[USBMON_HDR:USBMON_HDR+len_cap]
    return dict(id=urb_id, ev=ev_type.decode('latin1'), xtype=xfer_type,
                ep=epnum, dev=devnum, bus=busnum,
                setup_valid=(flag_setup == b'\x00'), setup=setup,
                status=status, length=length, data=payload)

XT = {0:'ISO', 1:'INT', 2:'CTRL', 3:'BULK'}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pcap'); ap.add_argument('--bus', type=int)
    ap.add_argument('--dev', type=int); ap.add_argument('--csv')
    ap.add_argument('--all', action='store_true', help='include non-control')
    ap.add_argument('--seq', help='write a pipe-separated op list: dir|bReq|wValue|wIndex|len|data')
    a = ap.parse_args()

    rows, seq, n, first = [], [], 0, None
    for ts, pkt in parse_pcap(a.pcap):
        r = decode(pkt)
        if not r: continue
        if a.bus and r['bus'] != a.bus: continue
        if a.dev and r['dev'] != a.dev: continue
        if r['xtype'] != 2 and not a.all: continue
        if first is None: first = ts
        if r['xtype'] == 2 and r['ev'] == 'C' and not r['setup_valid']:
            # completion: carries returned data for IN transfers
            n += 1
            d = r['data'][:24]
            hexd = ' '.join(f'{b:02x}' for b in d)
            print(f"[{n:5}] {ts-first:8.3f}s C  bus{r['bus']}.{r['dev']} "
                  f"{'':38} len={r['length']:<5} st={r['status']:<4} {hexd}")
            if a.seq:
                seq.append(('RET', None, None, None, r['length'], r['data']))
            rows.append((f"{ts-first:.6f}", 'C', r['bus'], r['dev'], '', '', '', '', '',
                         r['length'], r['status'], hexd))
        elif r['xtype'] == 2 and r['setup_valid']:
            bm, br, wv, wi, wl = struct.unpack('<BBHHH', r['setup'])
            n += 1
            d = r['data'][:24]
            hexd = ' '.join(f'{b:02x}' for b in d)
            direction = 'IN ' if bm & 0x80 else 'OUT'
            print(f"[{n:5}] {ts-first:8.3f}s {r['ev']} bus{r['bus']}.{r['dev']} "
                  f"{direction} bmReq=0x{bm:02x} bReq=0x{br:02x} "
                  f"wValue=0x{wv:04x} wIndex=0x{wi:04x} wLen={wl:<5} "
                  f"st={r['status']:<4} {hexd}")
            rows.append((f"{ts-first:.6f}", r['ev'], r['bus'], r['dev'], direction,
                         f"0x{bm:02x}", f"0x{br:02x}", f"0x{wv:04x}", f"0x{wi:04x}",
                         wl, r['status'], hexd))
            if a.seq and r['ev'] == 'S':
                seq.append(('IN' if bm & 0x80 else 'OUT', br, wv, wi, wl, r['data']))
        elif a.all:
            n += 1
            print(f"[{n:5}] {ts-first:8.3f}s {r['ev']} bus{r['bus']}.{r['dev']} "
                  f"{XT.get(r['xtype'],'?'):4} ep=0x{r['ep']:02x} len={r['length']} st={r['status']}")

    print(f"\n{n} records")
    if a.seq and seq:
        with open(a.seq, 'w') as f:
            for d, br, wv, wi, wl, data in seq:
                if d == 'RET':
                    f.write(f"RET|-|-|-|{wl}|{' '.join(f'{b:02x}' for b in data)}\n")
                else:
                    f.write(f"{d}|0x{br:02x}|0x{wv:04x}|0x{wi:04x}|{wl}|"
                            f"{' '.join(f'{b:02x}' for b in data)}\n")
        print(f"seq written: {a.seq} ({len(seq)} ops)")
    if a.csv and rows:
        import csv
        with open(a.csv,'w',newline='') as f:
            w = csv.writer(f)
            w.writerow(['t','ev','bus','dev','dir','bmRequestType','bRequest',
                        'wValue','wIndex','wLength','status','data'])
            w.writerows(rows)
        print(f"csv written: {a.csv}")

if __name__ == '__main__':
    main()
