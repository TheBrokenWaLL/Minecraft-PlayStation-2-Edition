#!/usr/bin/env python3
"""Import supplied SVG controls as 32px PNGs. Requires rsvg-convert.
Existing PNGs are never overwritten. No archive paths are extracted directly.
"""
import argparse
from pathlib import Path, PurePosixPath
import subprocess
import zipfile
import re

ALIASES = {
    'ps2': {'xis': ['cross'], 'circulo': ['circle'], 'quadrado': ['square'],
            'triangulo': ['triangle'], 'dpad': ['d_pad'],
            'dpad_cima': ['d_pad_up'], 'dpad_baixo': ['d_pad_down'],
            'dpad_esquerda': ['d_pad_left'], 'dpad_direita': ['d_pad_right']},
    'wii': {'dpad': ['d_pad'], 'plus': ['_'], 'minus': ['-']},
    'keyboard': {'escape': ['esc'], 'shift_left': ['lshift', 'left_shift'],
        'shift_right': ['rshift', 'right_shift'], 'ctrl_left': ['lcontrol', 'left_ctrl'],
        'ctrl_right': ['rcontrol', 'right_ctrl'], 'alt_left': ['lmenu', 'left_alt'],
        'alt_right': ['rmenu', 'right_alt'], 'arrow_up': ['up'], 'arrow_down': ['down'],
        'arrow_left': ['left'], 'arrow_right': ['right'], 'page_up': ['prior', 'pageup'],
        'page_down': ['next', 'pagedown'], 'caps_lock': ['capital'],
        'num_lock': ['numlock'], 'scroll_lock': ['scroll'], 'enter': ['return'],
        'backspace': ['back'], 'minus_underscore': ['minus'], 'equals_plus': ['equals'],
        'bracket_left': ['lbracket'], 'bracket_right': ['rbracket'], 'quote': ['apostrophe'],
        'grave_tilde': ['grave'], 'numpad_plus': ['add'], 'numpad_minus': ['subtract'],
        'numpad_multiply': ['multiply'], 'numpad_divide': ['divide'],
        'numpad_enter': ['numpadenter'], 'numpad_decimal': ['decimal']}
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[1] / 'data/assets/gui/controls')
    args = parser.parse_args()
    rendered = {}
    def render(platform, stem, svg):
        dest = args.output / platform / (stem + '.png')
        if dest.exists():
            raise ValueError(f'Refusing to overwrite {dest}')
        png = subprocess.check_output(['rsvg-convert', '-w', '32', '-h', '32'], input=svg)
        dest.parent.mkdir(parents=True, exist_ok=True)
        with dest.open('xb') as output:
            output.write(png)
    with zipfile.ZipFile(args.archive) as archive:
        for entry in archive.infolist():
            path = PurePosixPath(entry.filename)
            if path.suffix != '.svg' or len(path.parts) != 3:
                continue
            family = {'ps2': 'ps2', 'wii': 'wii', 'teclado': 'keyboard'}.get(path.parts[1])
            if not family or not re.fullmatch(r'[a-z0-9_]+', path.stem):
                raise ValueError(f'Unexpected icon path: {path}')
            if entry.file_size > 16384:
                raise ValueError('Unexpected SVG size')
            svg = archive.read(entry)
            # Supplied icons are self-contained geometry/text; disallow external references.
            if any(token in svg.lower() for token in (b'<!entity', b'<!doctype', b'href=', b'url(')):
                raise ValueError(f'External SVG content: {path}')
            rendered[family, path.stem] = svg
    for (family, stem), svg in rendered.items():
        names = ALIASES.get(family, {}).get(stem, [stem])
        if family == 'keyboard' and re.fullmatch('numpad_[0-9]', stem):
            names = [stem.replace('_', '')]
        # '+' and '-' both normalize to '_': keep fallback rather than a misleading icon.
        if family == 'wii' and stem in ('plus', 'minus'):
            names = [stem]
        for name in names:
            render(family, name, svg)
    # Combine supplied arrow artwork for the menu's two-direction navigation labels.
    for name, first, second in [('up_down', 'arrow_up', 'arrow_down'),
                                 ('left_right', 'arrow_left', 'arrow_right')]:
        def body(stem):
            return rendered['keyboard', stem].decode().split('>', 1)[1].rsplit('</svg>', 1)[0]
        svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">'
               '<g transform="translate(0 32) scale(.5)">' + body(first) + '</g>'
               '<g transform="translate(64 32) scale(.5)">' + body(second) + '</g></svg>')
        render('keyboard', name, svg.encode())
    print(f'Imported {len(rendered)} source icons plus filename aliases and navigation pairs.')

if __name__ == '__main__':
    main()
