"""Synthetic USBPcap framing and privacy checks; no hardware or raw fixtures."""
import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('reference', Path(__file__).parents[1] / 'scripts/summarize_apex6_reference.py')
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)
HEADER = bytes.fromhex('d4c3b2a1020004000000000000000000ffff0000f9000000')


def record(body, endpoint=0x83, transfer=1, address=5, time=10):
    packet = struct.pack('<HQIHBHHBBI', 27, 1, 0, 9, 1, 2, address, endpoint, transfer, len(body)) + body
    return struct.pack('<IIII', 1, time, len(packet), len(packet)) + packet


def descriptor():
    body = bytearray(18)
    body[:2] = b'\x12\x01'
    struct.pack_into('<HH', body, 8, 0x37d7, 0x2502)
    return record(body, transfer=2)


def frame(command, payload):
    body = bytearray(32)
    body[:4] = bytes([0x5a, 0xa5, command, len(payload) + 2])
    body[4:4 + len(payload)] = payload
    body[31] = sum(body[2:31]) & 255
    return body


class ReferenceTests(unittest.TestCase):
    def test_valid_sequence(self):
        data = HEADER + descriptor() + record(frame(0x53, b'\x01\x12\x00'), endpoint=3, time=20)
        data += record(frame(0x57, b'\x98' + b'\x80' * 24), endpoint=3, time=30)
        result = reference.summarize(data)
        self.assertEqual(result['mode_timeline'][0]['target'], 0x12)
        self.assertEqual(result['mode_timeline'][0]['mode'], 0)
        self.assertEqual(result['waveform_writes'], 1)
        self.assertEqual(result['nonneutral_waveforms'], 0)
        self.assertEqual(result['reply_attribution'], 'not inferred')

    def test_private_info_is_not_exported(self):
        data = HEADER + descriptor() + record(frame(4, b'PRIVATE_UNIT_1234'))
        result = reference.summarize(data)
        self.assertEqual(result['gpa6_counts'], {'rx_04': 1})
        self.assertNotIn('PRIVATE', str(result))
        self.assertNotIn('50524956415445', str(result))

    def test_pipelined_replies_remain_unattributed(self):
        data = HEADER + descriptor()
        for target in (2, 0x12):
            data += record(frame(0x53, bytes([1, target, 0])), endpoint=3)
        normal = bytes.fromhex('5aa553010000' + '00' * 25 + '54')
        anomalous = bytes.fromhex('5aa553000001' + '00' * 25 + '54')
        data += record(normal) + record(anomalous)
        result = reference.summarize(data)
        replies = [r for r in result['mode_timeline'] if r['direction'] == 'rx']
        self.assertEqual([(r['count'], r['value']) for r in replies], [(1, 0), (0, 1)])
        self.assertTrue(all(r['checksum_valid'] for r in replies))
        self.assertTrue(all('target' not in r for r in replies))
        self.assertEqual(result['reply_attribution'], 'not inferred')
        self.assertEqual(result['physical_recovery'], 'not inferred')

    def test_truncated_capture_rejected(self):
        valid = HEADER + descriptor()
        for cut in (1, 10, 23, 25, len(valid) - 1):
            with self.assertRaises(ValueError):
                reference.summarize(valid[:cut])

    def test_unrelated_device_rejected(self):
        with self.assertRaises(ValueError):
            reference.summarize(HEADER + descriptor() + record(b'', address=6))

    def test_missing_descriptor_rejected(self):
        with self.assertRaises(ValueError):
            reference.summarize(HEADER + record(b''))

    def test_bad_waveform_rejected(self):
        wave = frame(0x57, b'\x98' + b'\x80' * 24)
        wave[31] ^= 1
        with self.assertRaises(ValueError):
            reference.summarize(HEADER + descriptor() + record(wave, endpoint=3))


if __name__ == '__main__':
    unittest.main()
