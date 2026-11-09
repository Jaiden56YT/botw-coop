"""Extract the ExeFS (main, main.npdm, sdk, ...) from the program NCA of an NSP.

Uses the keys in ~/.switch/prod.keys. Run with the project's venv:
    .venv/bin/python -I tools/extract_exefs.py <file.nsp> <out_dir>
"""
import sys
from pathlib import Path

from nsz.Fs import Nsp, Nca, Pfs0, Ticket
from nsz.Fs.Type import Content
from nsz.nut import Keys, Titles


def main():
    nsp_path, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    Keys.load(str(Path.home() / ".switch" / "prod.keys"))

    nsp = Nsp.Nsp(str(nsp_path), "rb")

    # Register the title key from the NSP's ticket so rights-managed NCAs decrypt.
    for f in nsp:
        if isinstance(f, Ticket.Ticket):
            rights = f.getRightsId()
            rights_hex = format(rights, "032x") if isinstance(rights, int) else rights
            title_id = rights_hex[:16].upper()
            key = format(f.getTitleKeyBlock(), "032x")
            t = Titles.get(title_id)
            t.key = key
            Titles.set(title_id, t)
    nsp.close()

    nsp = Nsp.Nsp(str(nsp_path), "rb")
    for f in nsp:
        if not isinstance(f, Nca.Nca) or f.header.contentType != Content.PROGRAM:
            continue
        for fs in f:
            if not isinstance(fs, Pfs0.Pfs0):
                continue
            names = [x._path for x in fs.files]
            if "main" not in names:
                continue
            for entry in fs.files:
                entry.rewind()
                data = entry.read()
                (out_dir / entry._path).write_bytes(data)
                print(f"{entry._path}: {len(data)} bytes")
            return
    sys.exit("No ExeFS with a 'main' file found")


if __name__ == "__main__":
    main()
