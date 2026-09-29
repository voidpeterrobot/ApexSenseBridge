"""Offline USBPcap GPA6 timeline. No device access or reply attribution."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def reports(data):
    if data[:4] != b'\xd4\xc3\xb2\xa1' or len(data) < 24:
        raise ValueError('expected little-endian microsecond pcap')
    if struct.unpack_from('<I', data, 20)[0] != 249:
        raise ValueError('expected USBPcap link type')
    offset, number, origin = 24, 0, None
    while offset < len(data):
        if len(data)-offset < 16:
            raise ValueError('truncated pcap record')
        sec, usec, size, original = struct.unpack_from('<IIII', data, offset)
        packet = data[offset+16:offset+16+size]
        if len(packet) != size or size != original or len(packet) < 27:
            raise ValueError('truncated USBPcap packet')
        offset += 16+size
        number += 1
        timestamp = sec*1000000+usec
        if origin is None:
            origin = timestamp
        header = struct.unpack_from('<H', packet)[0]
        if not 27 <= header <= len(packet):
            raise ValueError('invalid USBPcap header')
        body = packet[header:]
        if len(body) != 32 or body[:2] != b'\x5a\xa5':
            continue
        if struct.unpack_from('<I', packet, 23)[0] != 32:
            raise ValueError('GPA6 data length mismatch')
        yield dict(frame=number, relative_us=timestamp-origin,
                   direction='device_to_host' if packet[21] & 128 else 'host_to_device',
                   padded_checksum_valid=(sum(body[2:31]) & 255) == body[31],
                   command=body[2], usb_body_hex=body.hex(), windows_report_hex='00'+body.hex())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = args.capture.read_bytes()
    result = dict(schema='asb.apex6.capture-timeline.v1', capture=args.capture.name,
                  sha256=hashlib.sha256(data).hexdigest(),
                  reply_attribution='not inferred', reports=list(reports(data)))
    with args.output.open('x', encoding='utf-8', newline='\n') as out:
        json.dump(result, out, indent=2)
        out.write('\n')
    for r in result['reports']:
        if r['direction'] == 'host_to_device':
            print(r['frame'], r['relative_us'], r['usb_body_hex'])


if __name__ == '__main__':
    main()
