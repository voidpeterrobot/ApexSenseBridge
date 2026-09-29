# Apex6 offline reference values

`ReferenceVectors.h` preserves the 71 quantizer and 48 waveform cases from
OpenFlydigi's `tests/fixtures/apex6-encoder.json`. These are encoder-generated
protocol vectors, not physical captures. Copyright 2026 Mikalai Kaliaha, MIT;
see `THIRD_PARTY_NOTICES.md` for the full terms. The source encoder fixture's
SHA-256 is `c66c0927d112fff629eefec2fd4f68378613e0c403b8f9742ca9d05b9aa00109`.

The four RAM blocks are **synthetic**, replacing historical retail readbacks.
IDs 1/4/5 use `(offset * 37 + id * 11) & 255` at lengths 82/22/222. RAM6 is
64 zero-initialized bytes with layout 2 and explicit left/right mode 1,
parameter `0x40` at offsets 23–26. CRC32 constants were calculated independently
of the codec under test. Chunk envelopes are also synthetic. These fixtures
exercise lengths, chunk assembly, CRC rejection and grip-layout decoding; they
are neither a physical baseline nor evidence that arbitrary RAM is valid on a
controller. Test identities, firmware words and configuration CRCs are synthetic.

`OfficialSilent.json` and its C++ companion contain only selected protocol
entry/exit/restore reports and relative timing from an official-app capture.
No descriptors, unit IDs, serials, host paths or configuration dumps are included.
Pipelined replies are not attributed to individual requests. The source-capture
digest allows private provenance checking; raw capture data is not distributed.

Entry/exit/neutral goldens in `test_apex6_offline.cpp` derive from OpenFlydigi's
`vectors/gpa6-reference.json`. DSP tests cover fragment invariance, channel
isolation, DC normalization, impulse delay, passband preservation and stopband
rejection; they do not claim bit-exact equivalence with Python floating-point DSP.
