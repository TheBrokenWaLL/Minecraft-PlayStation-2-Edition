#!/usr/bin/env python3
"""Recover PC/Wii OGG resources from PS2 APCM and raw PCM, non-destructively.

ADPCM math follows src/ps2/audio/Ps2AdpcmStreamDecoder.cpp. Raw PCM defaults
match Ps2MusicStream.cpp; use --pcm-channels 2 for known stereo source packs.
Requires FFmpeg with libvorbis and ffprobe. Existing outputs are never replaced.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import wave


def decode_adp(source, destination):
    data = source.read_bytes()
    if len(data) < 32 or data[:4] != b'APCM':
        raise ValueError('not an APCM audio file')
    version, channels = data[4:6]
    pitch, frames = struct.unpack_from('<II', data, 8)
    # Current SFX assets are mono; do not guess stereo channel plane alignment.
    if version != 1 or channels != 1 or not frames:
        raise ValueError('only version-1 mono APCM is supported')
    rates = (11025, 22050, 32000, 44100, 48000)
    rate = min(rates, key=lambda r: abs(r * 4096 // 48000 - pitch))
    if abs(rate * 4096 // 48000 - pitch) > 1:
        raise ValueError('unrecognized APCM sample rate')
    blocks = (frames + 27) // 28
    if (len(data) - 16) % 16 or len(data) < 16 + blocks * 16:
        raise ValueError('truncated/misaligned APCM payload')
    h1 = h2 = emitted = 0
    c1 = (0, 60, 115, 98, 122)
    c2 = (0, 0, -52, -55, -60)
    with wave.open(str(destination), 'wb') as out:
        out.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
        for offset in range(16, 16 + blocks * 16, 16):
            predictor, shift = data[offset] >> 4, data[offset] & 15
            if predictor >= 5 or shift > 12:
                raise ValueError('invalid APCM predictor/shift')
            decoded = []
            for byte in data[offset + 2:offset + 16]:
                for nibble in (byte & 15, byte >> 4):
                    signed = nibble - 16 if nibble & 8 else nibble
                    sample = ((signed * 4096) >> shift) + ((h1 * c1[predictor] + h2 * c2[predictor] + 32) >> 6)
                    sample = max(-32768, min(32767, sample))
                    h2, h1 = h1, sample
                    if emitted < frames:
                        decoded.append(sample)
                        emitted += 1
            out.writeframesraw(struct.pack('<' + 'h' * len(decoded), *decoded))
    return rate, 1, frames


def checked(command):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE).stdout


def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            checksum.update(block)
    return checksum.hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'data/resources_ps2')
    parser.add_argument('--output', type=Path, default=root / 'data/resources')
    parser.add_argument('--pcm-channels', type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    for tool in ('ffmpeg', 'ffprobe'):
        if not shutil.which(tool):
            parser.error(f'{tool} is required')
    if not args.source.is_dir():
        parser.error('source directory does not exist')
    sources = sorted(p for p in args.source.rglob('*') if p.suffix.lower() in ('.adp', '.pcm') and p.is_file())
    if not sources:
        parser.error('no PS2 audio found')
    targets = [args.output / p.relative_to(args.source).with_suffix('.ogg') for p in sources]
    if len(set(targets)) != len(targets):
        parser.error('multiple inputs map to the same OGG path')
    report = []
    for index, (source, target) in enumerate(zip(sources, targets), 1):
        if target.exists():
            report.append({'source': str(source), 'output': str(target), 'status': 'skipped-existing'})
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        before = digest(source)
        # Output temp lives on the destination filesystem; hard-link publication
        # is atomic and refuses any existing destination, even in a race.
        with tempfile.TemporaryDirectory(prefix='.audio-recovery-', dir=target.parent) as scratch:
            scratch = Path(scratch)
            if source.suffix.lower() == '.adp':
                wav = scratch / 'decoded.wav'
                rate, channels, frames = decode_adp(source, wav)
                input_args = ['-i', str(wav)]
            else:
                rate, channels = 22050, args.pcm_channels
                size = source.stat().st_size
                if not size or size % (2 * channels):
                    raise ValueError(f'invalid raw PCM length: {source}')
                frames = size // (2 * channels)
                input_args = ['-f', 's16le', '-ar', str(rate), '-ac', str(channels), '-i', str(source)]
            encoded = scratch / 'audio.ogg'
            checked(['ffmpeg', '-nostdin', '-v', 'error', '-n', *input_args,
                     '-map_metadata', '-1', '-c:a', 'libvorbis', '-q:a', '5', str(encoded)])
            probe = json.loads(checked(['ffprobe', '-v', 'error', '-show_streams', '-of', 'json', str(encoded)]))['streams'][0]
            if (probe['codec_name'] != 'vorbis' or int(probe['sample_rate']) != rate or
                    probe['channels'] != channels or abs(float(probe['duration']) - frames / rate) > 0.05):
                raise ValueError(f'output format/duration mismatch: {source}')
            checked(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-i', str(encoded), '-f', 'null', '-'])
            if digest(source) != before:
                raise RuntimeError(f'source changed during conversion: {source}')
            os.link(encoded, target)
        report.append({'source': str(source), 'output': str(target), 'status': 'converted',
                       'source_sha256': before, 'rate': rate, 'channels': channels, 'frames': frames})
        if index % 25 == 0:
            print(f'{index}/{len(sources)} converted', flush=True)
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', prefix='audio-recovery-', suffix='.json',
                                     dir=args.output.parent, delete=False) as log:
        json.dump(report, log, indent=2)
        print(f'Report: {log.name}')
    print(f'Converted {sum(r["status"] == "converted" for r in report)}; skipped {sum(r["status"] == "skipped-existing" for r in report)} existing files.')


if __name__ == '__main__':
    main()
