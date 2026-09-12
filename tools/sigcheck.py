r"""Check every Trinity byte signature against the installed game. Run after a patch.

    python tools/sigcheck.py

Reads the kSig_* constants straight out of the sources, so it cannot drift
from what the mod actually searches for, and gives one of four verdicts:

    OK        exactly one match   -> nothing to do
    AMBIG     more than one match -> fine only where Trinity pairs the pattern
                                     with a FindPatternIf predicate, otherwise
                                     it has to be tightened
    GONE      no match            -> that feature disables itself
    skipped   not a byte pattern  -> string anchors and the like

This answers "what did the patch break" before anyone spends a minute
guessing. TU 2.01.00 broke 30 of 47 signatures and took most of a day to
re-derive; TU 2.02.00 broke none, which this reported in under a minute.

What it CANNOT see, and what therefore still needs doing by hand after a
patch: the struct offsets. 2.01.00 moved kTls_RealmFlag 498 -> 509,
kOff_EquipComp_Table 0x80 -> 0x90, and a move-owner offset that lived only
inside a .cpp - three silent breakages, none visible to any byte scan. A
clean report here means the patterns still resolve, not that the mod is safe.

Needs pefile. The usual location is D:\dev\.tools\pydeps; override with
TRINITY_PYDEPS, and the game path with TRINITY_GAME_EXE.
"""
import os
import re
import sys

sys.path.insert(0, os.environ.get('TRINITY_PYDEPS', r'D:\dev\.tools\pydeps'))
import pefile

EXE = os.environ.get(
    'TRINITY_GAME_EXE',
    r'D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe')
SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src')

pe = pefile.PE(EXE, fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
# Executable sections only. Trinity's own scanner walks page protections, so a
# packer renaming .text (2.01.00 did) costs nothing here either.
CODE = [(s.Name.rstrip(b'\0').decode('latin1'), IB + s.VirtualAddress, s.get_data())
        for s in pe.sections if s.Characteristics & 0x20000000]


def to_rx(pat):
    return b''.join(b'.' if t.startswith('?') else re.escape(bytes([int(t, 16)]))
                    for t in pat.split())


def find(pat, limit=6):
    rx = re.compile(to_rx(pat), re.DOTALL)
    out = []
    for _, base, d in CODE:
        for m in rx.finditer(d):
            out.append(base + m.start())
            if len(out) >= limit:
                return out
    return out


# The literal is usually split across adjacent string pieces, so glue them.
DECL = re.compile(r'kSig_(\w+)\s*=\s*((?:"[^"]*"\s*)+);', re.S)

sigs = {}
for root, _, files in os.walk(SRC):
    for fn in files:
        if not fn.endswith(('.h', '.cpp')):
            continue
        p = os.path.join(root, fn)
        with open(p, encoding='utf-8', errors='replace') as fh:
            text = fh.read()
        for name, blob in DECL.findall(text):
            joined = ''.join(re.findall(r'"([^"]*)"', blob))
            sigs.setdefault(name, (joined, os.path.relpath(p, SRC)))

# Trinity's scanner takes both `?` and `??` as a wildcard byte; so does this.
BYTES_ONLY = re.compile(r'^(?:[0-9A-Fa-f]{2}|\?\??)(?:\s+(?:[0-9A-Fa-f]{2}|\?\??))*$')

ok, ambig, gone, skipped = [], [], [], []
for name in sorted(sigs):
    pat, where = sigs[name]
    if not BYTES_ONLY.match(pat.strip()):
        skipped.append((name, where, pat[:40]))
        continue
    hits = find(pat)
    if len(hits) == 1:
        ok.append((name, where, hits[0]))
    elif hits:
        ambig.append((name, where, hits))
    else:
        gone.append((name, where, pat))

print('game     : %s' % EXE)
print('sections : %s' % ', '.join('%s@%X' % (n, b) for n, b, _ in CODE))
print()
print('total %d byte signatures  |  OK %d   AMBIG %d   GONE %d   (skipped %d)'
      % (len(ok) + len(ambig) + len(gone), len(ok), len(ambig), len(gone), len(skipped)))

if gone:
    print('\n=== GONE (no match - feature disables itself) ===')
    for name, where, pat in gone:
        print('  kSig_%-26s %-16s %s' % (name, where, pat[:58]))
if ambig:
    print('\n=== AMBIGUOUS (>1 match) ===')
    print('  expected for LeaR8Rip, MovR8Rip, TableResolverPrologue and')
    print('  MarkerOriginPrefix - those are paired with a FindPatternIf')
    print('  predicate. Anything else here needs tightening.')
    for name, where, hits in ambig:
        print('  kSig_%-26s %-16s %d hits: %s'
              % (name, where, len(hits), ' '.join('%X' % h for h in hits[:4])))
if ok:
    print('\n=== OK ===')
    for name, where, a in ok:
        print('  kSig_%-26s %-16s %X' % (name, where, a))
if skipped:
    print('\n=== not byte patterns (string anchors etc.) ===')
    for name, where, head in skipped:
        print('  kSig_%-26s %-16s %s' % (name, where, head))

sys.exit(1 if gone else 0)
