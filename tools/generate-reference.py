#!/usr/bin/env python3
"""Generate the configuration and key reference from termica's C sources.

The runtime has no standalone schema: settings_known_key, statusbar_known_key,
module_id and the key tables define it. Defaults come from the corresponding
initializers. Fail on unfamiliar source shapes rather than publish a partial
reference. Python 3.8+; run from any directory.
"""

import argparse
import ast
import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def source(name):
    return (ROOT / 'src' / name).read_text(encoding='utf-8')


def function(text, name):
    match = re.search(r'\b' + re.escape(name) + r'\([^;]*?\)\s*\n\{(.*?)\n\}', text, re.S)
    if not match:
        raise ValueError('cannot find function ' + name)
    return match.group(1)


def require(pattern, text, label, flags=0):
    match = re.search(pattern, text, flags)
    if not match:
        raise ValueError('cannot find ' + label)
    return match


def quoted(text):
    return re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', text)


def c_string(text):
    return ast.literal_eval('"' + text + '"')


def code(value):
    return '`' + str(value).replace('`', '\\`').replace('|', '\\|').replace('\n', '\\n') + '`'


def write_table(lines, columns, rows):
    lines.extend(['| ' + ' | '.join(columns) + ' |',
                  '| ' + ' | '.join('---' for _ in columns) + ' |'])
    lines.extend('| ' + ' | '.join(row) + ' |' for row in rows)
    lines.append('')


def settings_reference(settings, header):
    known = function(settings, 'settings_known_key')
    arrays = dict(re.findall(r'static const char \*const (\w+)\[\] = \{([^}]*)\}', known))
    if set(arrays) != {'font', 'window', 'terminal', 'colors'}:
        raise ValueError('settings tables changed; update descriptions')
    tables = {name: quoted(value) for name, value in arrays.items()}
    dispatched = set(re.findall(r'key_is\(name,\s*"([^"]+)"\)', function(settings, 'apply_table')))
    if dispatched != set(tables) | {'keys'}:
        raise ValueError('settings table dispatch changed; update reference')
    palette = require(r'PALETTE_KEY_NAMES\[16\]\s*=\s*\{([^}]*)\}', settings,
                      'palette keys', re.S).group(1)
    tables['colors'] += quoted(palette)
    init = function(settings, 'settings_default')
    fmt = function(settings, 'settings_format')
    constants = dict(re.findall(r'^#define\s+(SETTINGS_\w+)\s+(\d+)\s*$', header, re.M))
    preset = require(r'colors_preset\(&settings->colors,\s*"([^"]+)"\)', init, 'default preset').group(1)

    # Descriptions describe semantics; names and defaults are read from C.
    notes = {
        'font.face': ('font', 'Empty uses the built-in font; otherwise an installed family or font file.'),
        'font.path': ('font', 'Legacy alias for `face`.'),
        'font.size': ('point_size', 'Number, greater than 0 and at most 200 points.'),
        'font.ligatures': ('ligatures', "Boolean; shape adjacent ASCII cells so the font's programming ligatures render."),
        'font.fallback': (None, 'Array of up to SETTINGS_MAX_FALLBACKS nonempty font families or paths; then OS and Symbols Nerd Font fallback.'),
        'window.opacity': ('panel_opacity', 'Number from 0 to 1.'),
        'window.backdrop': ('backdrop', '`panel`, `mica`, `mica_alt` (also `mica-alt`), or `acrylic`.'),
        'window.decoration': ('decoration', '`rounded`, `square`, `compact`, or `native`.'),
        'window.title_bar': ('title_bar', 'Boolean; ignored for the native frame.'),
        'terminal.copy_on_select': ('copy_on_select', 'Boolean; copy selected text automatically.'),
        'terminal.shell': ('shell', '`auto` selects pwsh, then powershell, then cmd; or choose `pwsh`, `powershell`, or `cmd`.'),
        'terminal.osc52': ('osc52', '`ask`, `allow`, or `deny` for OSC 52 clipboard reads and writes.'),
        'terminal.notifications': ('notifications', '`ask`, `allow`, or `deny` for OSC 9/777 notifications.'),
        'terminal.bell': ('bell', '`visual`, `taskbar`, or `none`.'),
        'terminal.command': (None, 'Array of up to SETTINGS_MAX_ARGS argument strings; nonempty argv[0]. Overrides `shell`; no shell string splitting.'),
        'terminal.cwd': (None, 'Working directory string; empty inherits the parent directory.'),
        'terminal.env': (None, 'Table of up to SETTINGS_MAX_ENV string variables overlaid on the child environment.'),
        'terminal.scrollback_lines': ('scrollback_lines', 'Integer from 0 to SETTINGS_SCROLLBACK_MAX; retained in page-sized increments.'),
        'colors.scheme': ('preset', '`dark`, `default` (alias for dark), or `light`; applied before individual overrides.'),
        'colors.palette': ('preset', 'Array of up to 16 `#RRGGBB` strings in ANSI order; entries override the scheme.'),
        'colors.foreground': ('preset', 'Default foreground; `#RRGGBB`.'),
        'colors.background': ('preset', 'Default background; `#RRGGBB`.'),
        'colors.cursor': ('preset', 'Cursor colour; `#RRGGBB`.'),
        'colors.selection_foreground': ('preset', 'Selection foreground; `#RRGGBB`.'),
        'colors.selection_fg': ('preset', 'Alias for `selection_foreground`.'),
        'colors.selection_background': ('preset', 'Selection background; `#RRGGBB`.'),
        'colors.selection_bg': ('preset', 'Alias for `selection_background`.'),
    }
    for key in quoted(palette):
        notes['colors.' + key] = ('preset', 'ANSI palette colour; `#RRGGBB`.')
    actual = {name + '.' + key for name, keys in tables.items() for key in keys}
    if set(notes) != actual:
        raise ValueError('settings descriptions differ from runtime keys: ' + str(set(notes) ^ actual))

    def default(field):
        if field is None:
            return 'unset'
        if field == 'preset':
            return code(preset + ' preset')
        if field == 'font' and not re.search(r'settings->font\b', init):
            require(r'memset\(settings,\s*0,\s*sizeof\s*\*settings\)', init, 'zeroed settings')
            return code('""')
        assignment = require(r'settings->' + field + r'\s*=\s*([^;]+);', init, 'default ' + field).group(1).strip()
        if assignment in constants:
            assignment = constants[assignment]
        if field in ('shell', 'osc52', 'notifications', 'bell', 'backdrop', 'decoration'):
            switch = require(r'switch\s*\(settings->' + field + r'\)\s*\{(.*?)\n\s*\}', fmt,
                             'format switch ' + field, re.S).group(1)
            cases = dict(re.findall(r'case\s+([\w]+)\s*:\s*' + field + r'\s*=\s*"([^"]+)"', switch))
            if assignment in cases:
                assignment = cases[assignment]
            else:
                assignment = require(r'const char \*' + field + r'\s*=\s*"([^"]+)"', fmt,
                                     'format default ' + field).group(1)
            return code('"' + assignment + '"')
        return code(assignment.rstrip('f'))

    lines = []
    for table, names in tables.items():
        lines.extend(['### `[' + table + ']`', ''])
        rows = []
        for name in names:
            field, desc = notes[table + '.' + name]
            for constant, value in constants.items():
                desc = re.sub(r'\b' + constant + r'\b', value, desc)
            rows.append((code(name), default(field), desc))
        write_table(lines, ('Key', 'Default', 'Meaning / accepted value'), rows)
    return lines


def statusbar_reference(status):
    ids = function(status, 'module_id')
    modules = re.findall(r'key_is\(name,\s*"([^"]+)"\)\s*\)\s*return\s+STATUSBAR_(\w+)', ids)
    if not modules:
        raise ValueError('no statusbar modules found')
    known = function(status, 'statusbar_known_key')
    common_part = known.split('if (id == STATUSBAR_', 1)[0]
    common = re.findall(r'key_is\(key,\s*"([^"]+)"\)', common_part)
    pieces = re.split(r'if \(id == STATUSBAR_(\w+)\)', known)
    module_keys = dict(zip(pieces[1::2], pieces[2::2]))
    if set(module_keys) != {identifier for _, identifier in modules} - {'GIT_BRANCH'}:
        raise ValueError('statusbar module keys changed; inspect generator')
    defaults = function(status, 'set_module_defaults')
    generic = defaults.split('m = &sb->modules[STATUSBAR_', 1)[0]
    chunks = re.split(r'm = &sb->modules\[STATUSBAR_(\w+)\];', defaults)
    module_defaults = dict(zip(chunks[1::2], chunks[2::2]))
    if set(module_defaults) != {identifier for _, identifier in modules}:
        raise ValueError('statusbar defaults changed; inspect generator')
    top = function(status, 'statusbar_default')

    def field_default(text, obj, field):
        pattern = r'snprintf\(' + obj + r'->' + field + r',\s*sizeof\s+' + obj + r'->' + field + r',\s*"%s",\s*"([^"\\]*(?:\\.[^"\\]*)*)"\)'
        matches = re.findall(pattern, text, re.S)
        if matches:
            return code('"' + c_string(matches[-1]) + '"')
        match = re.search(obj + r'->' + field + r'\s*=\s*([^;]+);', text)
        if match:
            return code(match.group(1).strip().rstrip('f'))
        if re.search(obj + r'->' + field + r'\[0\]\s*=\s*\'\\0\';', text):
            return code('""')
        return None

    lines = ['### Top-level settings', '']
    root_keys = re.findall(r'key_is\(key,\s*"(\w+)"\)', function(status, 'parse_toml').split('else if (apply_table', 1)[0])
    if len(root_keys) != 3:
        raise ValueError('statusbar top-level keys changed')
    rows = [(code(key), field_default(top, 'sb', key), 'String; layout for this container.') for key in root_keys]
    if any(row[1] is None for row in rows):
        raise ValueError('missing statusbar container default')
    write_table(lines, ('Key', 'Default', 'Meaning'), rows)
    face = field_default(top, 'sb', 'font')
    size = field_default(top, 'sb', 'point_size')
    if not face or not size:
        raise ValueError('missing statusbar font default')
    lines.extend(['### `[font]`', ''])
    write_table(lines, ('Key', 'Default', 'Meaning'), [
        (code('face'), face, 'Font family or file; empty uses the built-in font.'),
        (code('path'), face, 'Legacy alias for `face`.'),
        (code('size'), size, 'Number, 9 to 200 points inclusive.'),
    ])
    known_font = set(re.findall(r'key_is\(key,\s*"([^"]+)"\)', function(status, 'apply_font')))
    if known_font != {'face', 'path', 'size'}:
        raise ValueError('statusbar font keys changed')
    descriptions = {
        'format': 'Module template.', 'symbol': 'Symbol in the module template.',
        'style': 'Style applied by `$style`.',
        'truncation_length': 'Trailing directory components, 0 for the full path; 0–64.',
        'home_symbol': 'Replacement for the home directory.',
        'ahead': 'Git ahead marker.', 'behind': 'Git behind marker.',
        'diverged': 'Git diverged marker.', 'dirty': 'Git dirty marker.',
        'clean': 'Git clean marker.', 'conflicted': 'Git conflict marker.',
        'style_success': 'Successful command style.', 'style_failure': 'Failed command style.',
        'symbol_success': 'Successful command symbol.', 'symbol_failure': 'Failed command symbol.',
        'min_time': 'Minimum duration in milliseconds, 0–3600000.',
        'time_format': 'Local-time `strftime` format.',
        'idle': 'Resting glyph override; empty uses the built-in glyph.',
    }
    for name, identifier in modules:
        names = [key for key in common if key != 'symbol' or identifier != 'BUSY']
        names += re.findall(r'key_is\(key,\s*"([^"]+)"\)', module_keys.get(identifier, ''))
        if len(set(names)) != len(names) or any(key not in descriptions for key in names):
            raise ValueError('unknown statusbar module key for ' + name)
        lines.extend(['### `[' + name + ']`', ''])
        rows = []
        for key in names:
            field = 'min_time_ms' if key == 'min_time' else key
            value = (field_default(module_defaults[identifier], 'm', field) or
                     field_default(generic, 'm', field))
            if value is None:
                raise ValueError('missing default for ' + name + '.' + key)
            rows.append((code(key), value, descriptions[key]))
        write_table(lines, ('Key', 'Default', 'Meaning'), rows)
    return lines


def key_reference(keys):
    names_body = require(r'KEY_NAMES\[\]\s*=\s*\{(.*?)\n\};', keys, 'key names', re.S).group(1)
    names = re.findall(r'\{\s*"([^"]+)",\s*SDL_SCANCODE_(\w+)\s*\}', names_body)
    actions_body = require(r'ACTION_NAMES\[\]\s*=\s*\{(.*?)\n\};', keys, 'actions', re.S).group(1)
    actions = re.findall(r'\{\s*"([^"]+)",\s*KEY_ACTION_(\w+)\s*\}', actions_body)
    if not names or not actions:
        raise ValueError('empty key names or actions')
    reverse = {}
    for name, scancode in names:
        reverse.setdefault(scancode, name)
    action_by_id = {identifier: name for name, identifier in actions}
    defaults = function(keys, 'keys_default')
    bindings = re.findall(r'keys_set\(map,\s*SDL_SCANCODE_(\w+),\s*(\w+),\s*KEY_ACTION_(\w+)\s*\);', defaults)
    if not bindings or len(bindings) != len(re.findall(r'\bkeys_set\s*\(', defaults)):
        raise ValueError('unrecognised default binding syntax')
    mods = {'cs': 'Ctrl+Shift+', 'KEY_MOD_CTRL': 'Ctrl+', 'KEY_MOD_SHIFT': 'Shift+'}
    rows = []
    for scancode, mod, action in bindings:
        if scancode not in reverse or action not in action_by_id or mod not in mods:
            raise ValueError('unrecognised default binding ' + str((scancode, mod, action)))
        rows.append((code(mods[mod] + reverse[scancode]), code(action_by_id[action])))
    lines = ['## Default key bindings', '']
    write_table(lines, ('Chord', 'Action'), rows)
    lines.extend(['## Actions', '', ', '.join(code(name) for name, _ in actions) + '.', '',
                  '## Accepted key names', '', ', '.join(code(name) for name, _ in names) + '.', ''])
    return lines


def generate():
    settings, header, status, keys = (source(name) for name in ('settings.c', 'settings.h', 'statusbar.c', 'keys.c'))
    inputs = ('settings.c', 'settings.h', 'statusbar.c', 'statusbar.h',
              'keys.c', 'keys.h', 'colors.c', 'input.c')
    digest = hashlib.sha256()
    for name in inputs:
        digest.update((name + '\0' + source(name) + '\0').encode('utf-8'))
    max_settings = require(r'#define\s+SETTINGS_MAX_BYTES\s+(\d+)', settings, 'settings size limit').group(1)
    max_status = require(r'#define\s+STATUSBAR_MAX_BYTES\s+(\d+)', status, 'statusbar size limit').group(1)
    max_keys = require(r'#define\s+SETTINGS_MAX_KEY_OVERRIDES\s+(\d+)', header, 'key override limit').group(1)
    lines = [
        '# Configuration and key binding reference', '',
        '<!-- Generated by tools/generate-reference.py; do not edit this file. -->', '',
        '<!-- Source SHA-256: ' + digest.hexdigest() + ' -->', '',
        'Key names and defaults are generated from the settings, status bar and key-binding implementation. When those sources change, review the explanatory notes in `tools/generate-reference.py`, then regenerate with `python tools/generate-reference.py`; use `--check` to detect stale output.', '',
        'On Windows, files live in `%APPDATA%\\termica\\`: `termica.toml` and `statusbar.toml`. On other platforms they use `$XDG_CONFIG_HOME/termica/` or `~/.config/termica/`. On Windows both reload live. Invalid live edits retain the previous configuration; unknown keys are ignored. Initial loading tolerates individual invalid values. Maximum file sizes: termica.toml ' + max_settings + ' bytes; statusbar.toml ' + max_status + ' bytes.', '',
        'TOML types are strict; numeric fields accept integers or floats where noted. Setting names are case-insensitive and ignore `-` and `_`; values are not generally folded. Use literal strings for Windows paths, e.g. `\'C:\\work\'`, or escape backslashes.', '',
        '## `termica.toml`', '',
    ]
    lines += settings_reference(settings, header)
    lines.extend(['### `[keys]`', '',
                  'Each quoted chord maps to an action. Overrides add to or replace the defaults; `none` removes a binding. A maximum of ' + max_keys + ' overrides is accepted on live reload. For example:', '',
                  '```toml', '[keys]', '"Ctrl+V" = "paste"', '"Ctrl+Shift+Q" = "none"', '```', '',
                  'Chords consist of modifiers followed by one key, joined with `+`. Modifiers are `Ctrl` (`Control`), `Shift`, `Alt`, and `Super` (`Gui`, `Win`, `Meta`). Names ignore case, spaces, hyphens, and underscores. Bindings use physical SDL scancodes, not characters: `plus` and `equals` name the same physical key. `Ctrl+V` is unbound by default and reaches the shell. See the generated lists below.', '',
                  '## `statusbar.toml`', '',
                  'The three top-level format strings contain `$module` tokens and literals. Each module has a template: `$symbol` and `$value` are substituted, and `[text]($style)` applies its style. A space immediately following an empty module is dropped. Style tokens are `fg:#RRGGBB`, `bg:#RRGGBB`, `bold`, `italic`, `underline`, and `dim`.', '',
                  'Directory, git and command data depend on shell integration: source the matching script in [`shell/`](../shell/) after prompt initialization. The busy animation is fixed; only its resting icon is configurable.', ''])
    lines += statusbar_reference(status)
    lines += key_reference(keys)
    lines.extend(['## Search and mouse', '',
                  'In the search overlay, Enter/F3 moves to the next match; Shift+Enter/Shift+F3 moves to the previous match; Alt+C toggles case sensitivity, Alt+R toggles regex, and Escape closes search. These overlay shortcuts are not `[keys]` actions.', '',
                  'Drag, double-click, or triple-click to select cells, a word, or a line; Alt+drag selects a block. Shift+drag or wheel takes the mouse back from applications tracking it. Ctrl+click opens an allowed OSC 8 or detected HTTP, HTTPS, or mailto link.', ''])
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='fail if the committed reference is stale')
    args = parser.parse_args()
    output = ROOT / 'docs' / 'configuration.md'
    generated = generate()
    if args.check:
        if not output.exists() or output.read_text(encoding='utf-8') != generated:
            raise SystemExit('docs/configuration.md is stale; run python tools/generate-reference.py')
    else:
        with output.open('w', encoding='utf-8', newline='\n') as stream:
            stream.write(generated)


if __name__ == '__main__':
    main()
