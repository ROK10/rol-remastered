"""Strict text edits for archive entries (plain XML), used by mods' [entries] "patch:" scripts.

A patch script is a .py file in the mod folder that defines

    def patch(text: str) -> str

modmgr calls it with the entry's current text (the pristine entry with the patches of mods enabled earlier
already applied) and stores the result. Everything outside the edited spots stays byte-identical, so line
endings and formatting of the original file survive, and several mods can edit the same entry.

Every helper fails loudly (PatchError) when its anchor is missing or ambiguous: a game update or another mod
that moved the anchor must stop the build instead of producing a silently broken file.
"""
import re


class PatchError(Exception):
    pass


def newline_of(text):
    return '\r\n' if '\r\n' in text else '\n'


def fragment(text, block):
    """Re-indent nothing, only convert the fragment's line endings to the file's ones."""
    nl = newline_of(text)
    return block.replace('\r\n', '\n').replace('\n', nl)


def _one(text, needle, what):
    i = text.find(needle)
    if i < 0:
        raise PatchError(f'{what}: anchor not found: {needle[:80]!r}')
    if text.find(needle, i + 1) >= 0:
        raise PatchError(f'{what}: anchor is not unique: {needle[:80]!r}')
    return i


def replace_once(text, old, new):
    i = _one(text, old, 'replace_once')
    return text[:i] + new + text[i + len(old):]


def insert_before(text, anchor, block):
    i = _one(text, anchor, 'insert_before')
    return text[:i] + fragment(text, block) + text[i:]


def insert_after(text, anchor, block):
    i = _one(text, anchor, 'insert_after') + len(anchor)
    return text[:i] + fragment(text, block) + text[i:]


def start_tag(text, tag, **match):
    """Span (start, end) of the single start tag <tag ...> whose attributes include all of `match`."""
    hits = []
    for m in re.finditer(r'<%s(\s[^>]*)?/?>' % re.escape(tag), text):
        attrs = dict(re.findall(r'([\w:.-]+)="([^"]*)"', m.group(1) or ''))
        if all(attrs.get(k) == v for k, v in match.items()):
            hits.append(m.span())
    if len(hits) != 1:
        raise PatchError(f'start_tag: <{tag} {match}> matched {len(hits)} times')
    return hits[0]


def set_attrs(text, tag, match, values):
    """Set attribute values on the single <tag> whose attributes include `match` (dict).

    Only existing attributes are changed (a typo must not add a new, ignored attribute)."""
    a, b = start_tag(text, tag, **match)
    head = text[a:b]
    for name, value in values.items():
        pat = r'(\s%s=")[^"]*(")' % re.escape(name)
        head, n = re.subn(pat, lambda m: m.group(1) + str(value) + m.group(2), head, count=1)
        if n != 1:
            raise PatchError(f'set_attrs: <{tag} {match}> has no attribute {name!r}')
    return text[:a] + head + text[b:]


def xml_text(s):
    """Escape a Python string for an XML attribute: non-ASCII becomes numeric character references."""
    s = s.replace('&', '&amp;').replace('"', '&quot;').replace('<', '&lt;').replace('>', '&gt;')
    return ''.join(c if ord(c) < 128 else f'&#{ord(c)};' for c in s)
