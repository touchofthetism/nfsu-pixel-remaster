"""Binarius endscripts, the subset texture mods use, read into texture overlays (hdpack.py --overlay).

A Binarius "User mode" mod ships a launcher file (``[VERSN1]`` and JSON naming the endscript, the game and
the files it opens) and an endscript (``[VERSN2]``, one command per line). Plajer's HD User Interface, for
one, replaces textures with

    bind_textures synchronize FRONTEND\\FRONTB.LZC TPKBlocks FRONTENDTEXTURES "Textures\\FrontB\\FRONTENDTEXTURES"

Every DDS in the folder replaces the texture of that texture pack whose name hash (record +0x24) is the hash of
the file's stem, or is added to the pack when none has it (Endscript's BindTexturesCommand: ``synchronize``
reloads the texture in place, ``override`` removes and adds it). A pack's collection name is the stem of the
path in its header (``Global\\FrontEndTexturesPC.tpk`` -> FRONTENDTEXTURESPC).

Read here:
  * ``bind_textures synchronize|override <file> TPKBlocks <collection> "<folder>"`` -> a :class:`Binding`;
  * ``combobox "<option>"... "<question>"`` with its ``"<option>"`` sections: only the chosen option's
    commands count (``Install``);
  * ``if collection_exists <file> TPKBlocks <collection> / do / ... / else / ... / end``: kept as
    :class:`Conditional` with the commands of each side, for the builder to decide against its files (the HD
    User Interface repairs a TexEd-damaged pack named DEFAULT that way; the stock files have none);
  * ``update_collection <file> TPKBlocks <collection> CompressionType <value>`` inside such a block;
  * ``move_file ... <plugin>.asi ...``: the PC plugin the mod needs, reported in :attr:`Script.plugins`
    (this port has their fixes in its runtime: hdpack.py maps a plugin to the app build that has it).

Ignored, as they change nothing in the game's data here: ``version``, ``unlock_memory`` (Binarius' own
memory), ``delete`` (unloads a file from Binarius once it is saved), ``create_folder``, ``speedreflect``
(Binarius' own plugin), ``watermark``. Anything else in the chosen branch is an error, so a mod that needs more
than texture replacement is never half-applied.
"""
from __future__ import annotations

import dataclasses
import json
import pathlib
import re
from typing import List, Optional, Tuple

IGNORED = {"version", "unlock_memory", "delete", "create_folder", "speedreflect", "watermark"}


@dataclasses.dataclass
class Binding:
    game_file: str          # game-relative with forward slashes, as the script writes it (case kept)
    collection: str         # the texture pack's collection name, upper case (FRONTENDTEXTURES)
    folder: pathlib.Path    # the folder of DDS files
    mode: str               # synchronize or override


@dataclasses.dataclass
class UpdateCollection:
    game_file: str
    collection: str
    field: str
    value: str


@dataclasses.dataclass
class Conditional:
    game_file: str          # collection_exists <file> TPKBlocks <collection>
    collection: str
    then: list
    otherwise: list


@dataclasses.dataclass
class Script:
    path: pathlib.Path
    game: str               # from the launcher file ("Underground1"), "" when the endscript was given directly
    bindings: List[Binding]
    conditionals: List[Conditional]
    plugins: List[str]      # file names of the PC plugins the mod copies (NFSUHDContentSupport.asi)
    watermark: str
    source: Optional[pathlib.Path] = None   # the file parse() was given: the launcher, or the endscript itself

    @property
    def label(self) -> str:
        """The mod's own name for logs: its launcher file's name (HD_User_Interface.end)."""
        return (self.source or self.path).name


class EndscriptError(ValueError):
    pass


def tokenize(line: str) -> List[str]:
    """Splits a line at whitespace, keeping double-quoted strings whole (quotes removed); ``//`` starts a comment."""
    tokens: List[str] = []
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if c.isspace():
            i += 1
        elif line.startswith("//", i):
            break
        elif c == '"':
            end = line.find('"', i + 1)
            if end < 0:
                raise EndscriptError(f"unterminated string: {line!r}")
            tokens.append(line[i + 1:end])
            i = end + 1
        else:
            j = i
            while j < n and not line[j].isspace() and not line.startswith("//", j):
                j += 1
            tokens.append(line[i:j])
            i = j
    return tokens


def _game_path(path: str) -> str:
    return path.replace("\\", "/")


def read_launcher(path: pathlib.Path) -> Tuple[pathlib.Path, str]:
    """(endscript path, game) from a ``[VERSN1]`` launcher file. Binarius writes Windows paths unescaped in its
    JSON (``"Assets\\main.end"`` with one backslash), so backslashes are read as path separators."""
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    if not text.lstrip().startswith("[VERSN1]"):
        raise EndscriptError(f"{path.name}: not a Binarius launcher file ([VERSN1])")
    body = text.split("]", 1)[1].replace("\\", "/")
    try:
        data = json.loads(body)
    except json.JSONDecodeError as error:
        raise EndscriptError(f"{path.name}: launcher JSON: {error}") from error
    if data.get("Usage") != "User":
        raise EndscriptError(f"{path.name}: only User mode mods are supported (Usage {data.get('Usage')!r})")
    endscript = data.get("Endscript")
    if not endscript:
        raise EndscriptError(f"{path.name}: no endscript named")
    return path.parent / endscript, data.get("Game", "")


def _resolve(base: pathlib.Path, relative: str) -> pathlib.Path:
    """A path under ``base`` written with Windows separators, matched case-insensitively."""
    current = base
    for part in _game_path(relative).split("/"):
        if not part:
            continue
        candidate = current / part
        if not candidate.exists() and current.is_dir():
            matches = [child for child in current.iterdir() if child.name.lower() == part.lower()]
            if matches:
                candidate = matches[0]
        current = candidate
    return current


def parse(path: pathlib.Path, option: str = "Install") -> Script:
    """Reads a launcher file or an endscript. ``option``: the combobox answer to follow."""
    path = source = pathlib.Path(path)
    game = ""
    if path.read_bytes()[:16].lstrip().startswith(b"[VERSN1]"):
        path, game = read_launcher(path)
    lines = path.read_text(encoding="utf-8-sig", errors="replace").splitlines()
    if not lines or lines[0].strip() != "[VERSN2]":
        raise EndscriptError(f"{path.name}: not a Binarius endscript ([VERSN2])")
    root = path.parent
    bindings: List[Binding] = []
    conditionals: List[Conditional] = []
    plugins: List[str] = []
    watermark = ""

    # A stack of open blocks: ("if", Conditional, side) or ("combobox", options, current option or None).
    stack: list = []

    def active() -> bool:
        """Whether a command here runs: inside a combobox only in the chosen option's section."""
        for block in stack:
            if block[0] == "combobox" and block[2] != option:
                return False
        return True

    def target() -> Optional[list]:
        """The command list of the innermost if-side, None at the top level."""
        for block in reversed(stack):
            if block[0] == "if":
                return block[1].then if block[2] == "then" else block[1].otherwise
        return None

    for number, raw in enumerate(lines[1:], start=2):
        tokens = tokenize(raw)
        if not tokens:
            continue
        command = tokens[0]
        where = f"{path.name}:{number}"
        # A lone quoted word inside a combobox starts that option's section.
        if stack and stack[-1][0] == "combobox" and len(tokens) == 1 and raw.strip().startswith('"'):
            if command not in stack[-1][1]:
                raise EndscriptError(f"{where}: section {command!r} is not one of the combobox options")
            stack[-1] = ("combobox", stack[-1][1], command)
            continue
        if command == "combobox":
            if len(tokens) < 3:
                raise EndscriptError(f"{where}: combobox needs options and a question")
            options = tokens[1:-1]
            if option not in options:
                raise EndscriptError(f"{where}: option {option!r} is not offered ({options})")
            stack.append(("combobox", options, None))
            continue
        if command == "if":
            if len(tokens) != 5 or tokens[1] != "collection_exists" or tokens[3] != "TPKBlocks":
                raise EndscriptError(f"{where}: unsupported condition {' '.join(tokens[1:])!r}")
            block = Conditional(_game_path(tokens[2]), tokens[4].upper(), [], [])
            if active():
                parent = target()
                (parent if parent is not None else conditionals).append(block)
            stack.append(("if", block, "cond"))
            continue
        if command in ("do", "else"):
            if not stack or stack[-1][0] != "if":
                raise EndscriptError(f"{where}: {command} outside an if")
            stack[-1] = ("if", stack[-1][1], "then" if command == "do" else "otherwise")
            continue
        if command == "end":
            if not stack:
                raise EndscriptError(f"{where}: end without a block")
            stack.pop()
            continue
        if not active():
            continue
        if command in IGNORED:
            if command == "watermark" and len(tokens) > 1:
                watermark = tokens[1]
            continue
        if command == "bind_textures":
            if len(tokens) != 6 or tokens[1] not in ("synchronize", "override") or tokens[3] != "TPKBlocks":
                raise EndscriptError(f"{where}: unsupported bind_textures form {raw.strip()!r}")
            folder = _resolve(root, tokens[5])
            if not folder.is_dir():
                raise EndscriptError(f"{where}: texture folder {tokens[5]!r} not found next to the endscript")
            binding = Binding(_game_path(tokens[2]), tokens[4].upper(), folder, tokens[1])
            parent = target()
            (parent if parent is not None else bindings).append(binding)
            continue
        if command == "update_collection":
            # update_collection <file> TPKBlocks <collection> <field> <value>
            if len(tokens) != 6 or tokens[2] != "TPKBlocks":
                raise EndscriptError(f"{where}: unsupported update_collection form {raw.strip()!r}")
            parent = target()
            if parent is None:
                raise EndscriptError(f"{where}: update_collection outside a collection_exists block is not supported")
            parent.append(UpdateCollection(_game_path(tokens[1]), tokens[3].upper(), tokens[4], tokens[5]))
            continue
        if command == "move_file":
            plugin = next((t for t in reversed(tokens) if t.lower().endswith(".asi")), None)
            if plugin is None:
                raise EndscriptError(f"{where}: move_file of something other than a plugin: {raw.strip()!r}")
            name = _game_path(plugin).rsplit("/", 1)[-1]
            if name not in plugins:
                plugins.append(name)
            continue
        raise EndscriptError(f"{where}: command {command!r} is not supported here")
    if stack:
        raise EndscriptError(f"{path.name}: {len(stack)} block(s) left open at the end")
    return Script(path, game, bindings, conditionals, plugins, watermark, source)


_STEM = re.compile(r"[^\\/]+$")


def collection_name(pack_path: str) -> str:
    """A texture pack's collection name: the stem of its header path, upper case."""
    stem = _STEM.search(pack_path).group(0)
    return stem.rsplit(".", 1)[0].upper()
