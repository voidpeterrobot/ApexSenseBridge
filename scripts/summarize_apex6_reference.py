"""Offline, identity-free summary of one address-filtered USBPcap reference.

Raw captures remain private. Reply attribution and physical recovery are never
inferred. Reject partial records instead of treating a forced stop as complete.
"""
import argparse
import collections
import hashlib
import json
import struct
from pathlib import Path


def packets(data):
    if len(data) < 24 or data[:4] != b'\xd4\xc3\xb2\xa1':
        raise ValueError('expected little-endian microsecond pcap')
    if struct.unpack_from('<I', data, 20)[0] != 249:
        raise ValueError('expected USBPcap link type')
    offset, number = 24, 0
    while offset < len(data):
        if len(data) - offset < 16:
            raise ValueError('truncated pcap record header')
        sec, usec, size, original = struct.unpack_from('<IIII', data, offset)
        offset += 16
        packet = data[offset:offset + size]
        offset += size
        number += 1
        if usec >= 1000000 or size != original or len(packet) != size or size < 27:
            raise ValueError('invalid/truncated pcap record')
        header = struct.unpack_from('<H', packet)[0]
        if not 27 <= header <= size:
            raise ValueError('invalid USBPcap header size')
        body = packet[header:]
        if struct.unpack_from('<I', packet, 23)[0] != len(body):
            raise ValueError('USBPcap data length mismatch')
        yield dict(frame=number, time_us=sec * 1000000 + usec,
                   status=struct.unpack_from('<I', packet, 10)[0],
                   info=packet[16], bus=struct.unpack_from('<H', packet, 17)[0],
                   address=struct.unpack_from('<H', packet, 19)[0],
                   endpoint=packet[21], transfer=packet[22], body=body)


def summarize(data):
    addresses, descriptors, counts = set(), set(), collections.Counter()
    timeline, waves, errors = [], [], 0
    origin, last, records = None, None, 0
    for p in packets(data):
        records += 1
        origin = p['time_us'] if origin is None else origin
        last = p['time_us']
        addresses.add((p['bus'], p['address']))
        body = p['body']
        if p['status']:
            errors += 1
        if p['transfer'] == 2 and len(body) == 18 and body[:2] == b'\x12\x01':
            descriptors.add(struct.unpack_from('<HH', body, 8))
        if len(body) != 32 or body[:2] != b'\x5a\xa5':
            continue
        direction = 'rx' if p['endpoint'] & 128 else 'tx'
        command = body[2]
        counts[f'{direction}_{command:02x}'] += 1
        base = dict(frame=p['frame'], relative_us=p['time_us'] - origin,
                    direction=direction, usb_status=p['status'])
        if command == 0x53:
            base['body_hex'] = body.hex()
            base['checksum_valid'] = sum(body[2:31]) & 255 == body[31]
            if direction == 'tx':
                size = body[3] - 2
                if not 3 <= size <= 27:
                    raise ValueError('invalid mode request length')
                base.update(area=body[4], target=body[5], mode=body[6],
                            parameters_hex=body[7:4 + size].hex())
            else:
                base.update(count=body[3], index=body[4], value=body[5])
            timeline.append(base)
        elif command == 0x57 and direction == 'tx':
            if body[3] != 27 or sum(body[2:31]) & 255 != body[31]:
                raise ValueError('invalid waveform framing/checksum')
            base.update(trigger_enabled=bool(body[4] & 4),
                        neutral=all(v == 128 for v in body[5:29]))
            waves.append(base)
    if len(addresses) != 1 or descriptors != {(0x37d7, 0x2502)}:
        raise ValueError('reference must contain exactly one address and the Apex6 model descriptor')
    return dict(schema='asb.apex6.usb-reference-summary.v1',
                capture_sha256=hashlib.sha256(data).hexdigest(), records=records,
                duration_us=last - origin, descriptor_verified=True,
                nonzero_usb_status_records=errors, gpa6_counts=dict(sorted(counts.items())),
                mode_timeline=timeline, waveform_writes=len(waves),
                nonneutral_waveforms=sum(not w['neutral'] for w in waves),
                trigger_enabled_waveforms=sum(w['trigger_enabled'] for w in waves),
                first_waveform_us=waves[0]['relative_us'] if waves else None,
                last_waveform_us=waves[-1]['relative_us'] if waves else None,
                reply_attribution='not inferred', physical_recovery='not inferred')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.capture.stat().st_size > 40 * 1024 * 1024:
        raise ValueError('capture exceeds diagnostic size limit')
    result = summarize(args.capture.read_bytes())
    with args.output.open('x', encoding='utf-8', newline='\n') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
