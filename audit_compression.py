"""Lossless, individually compressed audit CSVs: XZ/LZMA2 extreme settings."""

import hashlib
import lzma
import zipfile

FILTERS = [
    {
        "id": lzma.FILTER_LZMA2,
        "preset": 9 | lzma.PRESET_EXTREME,
        "dict_size": 64 * 1024 * 1024,
        "lc": 4,
        "lp": 0,
        "pb": 0,
    }
]
SETTINGS = "xz --threads=1 --check=crc64 --lzma2=preset=9e,dict=64MiB,lc=4,lp=0,pb=0"


def compress_stream(source, destination):
    """Write one XZ stream; return the uncompressed SHA-256 and byte count."""
    h = hashlib.sha256()
    size = 0
    with lzma.LZMAFile(
        destination,
        "wb",
        format=lzma.FORMAT_XZ,
        check=lzma.CHECK_CRC64,
        filters=FILTERS,
    ) as dst:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            dst.write(chunk)
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


class XZMember(lzma.LZMAFile):
    def __init__(self, archive, name):
        self._zip_member = archive.open(name)
        super().__init__(self._zip_member, "rb")

    def close(self):
        try:
            super().close()
        finally:
            self._zip_member.close()


def open_member(archive, name):
    """Open CSV bytes, transparently decoding individually compressed audits.
    Legacy plain audit members remain readable for historical comparisons.
    """
    if (
        name.endswith(".target_resolution_audit.csv")
        and name + ".xz" in archive.namelist()
    ):
        return XZMember(archive, name + ".xz")
    return archive.open(name)
