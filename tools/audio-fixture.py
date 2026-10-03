#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a known audio fixture through native AudioOut or the software decoder probe.

Stop video playback first. Requires tools/make-audio-fixtures.py output.
AAC/AC-3 capture blocks submitted to AudioOut. E-AC-3/DTS/TrueHD capture
software-decoded PCM without speaker output. Neither captures physical HDMI.
"""
import argparse
import io
import json
import os
from pathlib import Path
import time
from ftplib import FTP, error_perm
import wave
from console_config import console_host
import numpy as np

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name', choices=['aac_20', 'aac_51', 'ac3_20', 'ac3_51', 'eac3_20', 'eac3_51', 'dts_51', 'truehd_51'])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--sink', action='store_true', help='Send software decode through production AudioOut')
    parser.add_argument('--source', type=Path, help='Use a local encoded excerpt instead of generated tones')
    parser.add_argument('--programme-reference', type=Path, help='Host-decoded S16 WAV for an excerpt comparison')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if bool(args.source) != bool(args.programme_reference):
        parser.error('--source and --programme-reference must be supplied together')
    source = args.source or root / 'build/audio-fixtures' / (args.name + '.bin')
    software = args.name.startswith(("eac3", "dts", "truehd")) and not args.sink
    capture = "slopfin-software-audio" if software else "slopfin-audio"
    encoded = source.read_bytes()
    out = args.output or Path('/tmp/slopfin-' + args.name + '.wav')

    def connect():
        f = FTP(); f.connect(console_host(), 2121, timeout=20); f.login()
        return f

    with connect() as f:
        try: f.mkd('/data/slopfin-codec')
        except error_perm as exc:
            if not str(exc).startswith('550'): raise
        for path in [capture + '.raw', capture + '.txt', 'slopfin-audio-caps.txt',
                     'slopfin-audio-fixture', 'slopfin-audio-caps']:
            try: f.sendcmd('DELE /data/' + path)
            except error_perm as exc:
                if not str(exc).startswith('550'): raise
        f.storbinary('STOR /data/slopfin-codec/' + args.name + '.bin', io.BytesIO(encoded))
        f.storbinary('STOR /data/slopfin-audio-fixture', io.BytesIO((('out:' if args.sink else '') + args.name).encode()))
        f.storbinary('STOR /data/slopfin-audio-caps', io.BytesIO(b'1'))
    report = ''
    for _ in range(30):
        time.sleep(1)
        with connect() as f:
            b = bytearray()
            try: f.retrbinary('RETR /data/slopfin-audio-caps.txt', b.extend)
            except error_perm: continue
        report = b.decode(errors='replace').strip()
        if report: break
    if ('decoded,' if software else 'played=') not in report:
        raise SystemExit(report or 'Fixture did not complete; stop video playback before running it.')
    print(report)
    raw, side = bytearray(), bytearray()
    with connect() as f:
        f.retrbinary('RETR /data/' + capture + '.raw', raw.extend)
        f.retrbinary('RETR /data/' + capture + '.txt', side.extend)
    meta = dict(line.split() for line in side.decode().splitlines())
    channels, rate, frames = (int(meta[k]) for k in ['channels', 'rate', 'frames'])
    if rate != 48000 or frames < 48000 or len(raw) != channels * frames * 2:
        raise SystemExit('Incomplete fixture capture or unexpected rate.')
    with wave.open(str(out), 'wb') as f:
        f.setnchannels(channels); f.setframerate(rate); f.setsampwidth(2); f.writeframes(raw)
    pcm = np.frombuffer(raw, dtype='<i2').reshape(-1, channels).astype(float)
    if args.programme_reference:
        with wave.open(str(args.programme_reference), 'rb') as ref:
            ref_channels = ref.getnchannels()
            if ref.getsampwidth() != 2 or ref.getframerate() != rate or ref_channels > channels:
                raise SystemExit('Reference must be S16 at the captured rate with no more channels than output.')
            reference = np.frombuffer(ref.readframes(ref.getnframes()), dtype='<i2').reshape(-1, ref_channels).astype(float)
        # Align an independent host decode against the actually submitted PCM.
        # FFT avoids quadratic work on multi-second captures.
        nfft = 1 << (len(pcm) + len(reference) - 1).bit_length()
        correlation = np.fft.irfft(np.fft.rfft(pcm[:, 0], nfft) * np.conj(np.fft.rfft(reference[:, 0], nfft)), nfft)
        lag = int(np.argmax(correlation))
        if lag > nfft // 2: lag -= nfft
        a, b = max(lag, 0), max(-lag, 0)
        count = min(len(pcm) - a, len(reference) - b)
        if count < rate:
            raise SystemExit('Less than one second of aligned reference overlap.')
        actual, expected = pcm[a:a+count, :ref_channels], reference[b:b+count]
        print(f'Reference alignment: {lag} samples; overlap {count/rate:.3f} seconds.')
        passed = True
        for channel in range(ref_channels):
            x, y = actual[:, channel], expected[:, channel]
            rms = float(np.sqrt(np.mean(y*y)))
            error = float(np.sqrt(np.mean((x-y)**2)))
            norm = error / max(1, rms)
            corr = float(np.corrcoef(x,y)[0,1]) if rms > 1 else 1.0
            good = norm < 0.05 and corr > 0.99
            passed &= good
            print(f'Channel {channel+1}: reference RMS={rms:.2f}, error RMS={error:.2f}, relative error={norm:.4f}, correlation={corr:.5f}: {"PASS" if good else "FAIL"}')
        scope = 'software-decoded PCM with no AudioOut submission' if software else 'submitted AudioOut PCM'
        print(f'Wrote {out}. Waveform comparison covers {scope}; physical HDMI processing remains outside this capture.')
        return 0 if passed else 1
    pcm = pcm[rate // 4: min(frames, rate * 2)]
    manifest = json.loads((source.parent / 'manifest.json').read_text())
    expected = next(case['tone_hz'] for case in manifest['cases'] if case['name'] == args.name)
    if args.sink and args.name.startswith(('eac3', 'dts', 'truehd')) and len(expected) == 6 and channels == 8:
        # FFmpeg 5.1(side) uses side slots; the fixture manifest follows source order.
        import subprocess
        layout = subprocess.check_output(['ffprobe', '-v', 'error', '-show_entries',
            'stream=channel_layout', '-of', 'default=nw=1:nk=1', str(source)], text=True).strip()
        if layout == '5.1(side)': expected = expected[:4] + [0, 0] + expected[4:]
    names = ['FL', 'FR', 'FC', 'LFE', 'BL', 'BR', 'SL', 'SR']
    if software and channels == 6:
        names[4:6] = ['surround L', 'surround R']
    passed = channels >= len(expected)
    for channel in range(channels):
        samples = pcm[:, channel]
        level = float(np.sqrt(np.mean(samples * samples)))
        spectrum = abs(np.fft.rfft(samples * np.hanning(len(samples))))
        hz = float(np.argmax(spectrum) * rate / len(samples))
        target = expected[channel] if channel < len(expected) else 0
        good = abs(hz - target) < 2 and level > 100 if target else level < 10
        passed &= good
        print(f'{names[channel]}: {hz:.1f} Hz, RMS {level:.1f}, expected {target or "silence"}: {"PASS" if good else "FAIL"}')
    if software:
        with wave.open(str(source.with_suffix('.reference.wav')), 'rb') as reference:
            reference_pcm = np.frombuffer(reference.readframes(reference.getnframes()), dtype='<i2').astype(float)
            same_format = reference.getnchannels() == channels and reference.getframerate() == rate
        actual = np.frombuffer(raw, dtype='<i2').astype(float)
        same_length = len(actual) == len(reference_pcm)
        rms_error = float(np.sqrt(np.mean((actual - reference_pcm) ** 2))) if same_length else float('inf')
        reference_ok = same_format and same_length and rms_error < 8
        passed &= reference_ok
        print(f'Host reference: same format={same_format}, same length={same_length}, RMS error={rms_error:.3f}: {"PASS" if reference_ok else "FAIL"}')
    scope = 'software-decoded PCM; no speaker output' if software else 'submitted AudioOut PCM; physical speakers still require listening'
    print(f'Wrote {out}. This verifies {scope}.')
    return 0 if passed else 1

if __name__ == '__main__':
    raise SystemExit(main())
