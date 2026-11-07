"""PE32 image loader: sections, relocations, imports, exports as a flat virtual image."""
import struct
import pefile


class Image:
    def __init__(self, path, base=None):
        pe = pefile.PE(path)
        if base is not None and base != pe.OPTIONAL_HEADER.ImageBase:
            pe.relocate_image(base)  # applies .reloc fixups and updates ImageBase
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.mem = bytearray(self.size)
        hdr = pe.OPTIONAL_HEADER.SizeOfHeaders
        self.mem[:hdr] = pe.__data__[:hdr]
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            va = self.base + s.VirtualAddress
            data = s.get_data()[: max(s.Misc_VirtualSize, 0) or s.SizeOfRawData]
            off = s.VirtualAddress
            self.mem[off : off + len(data)] = data
            self.sections.append((name, va, max(s.Misc_VirtualSize, s.SizeOfRawData), s.Characteristics))
        code = [s for s in self.sections if s[3] & 0x20000000]  # IMAGE_SCN_MEM_EXECUTE
        self.code_lo = min(s[1] for s in code)
        self.code_hi = max(s[1] + s[2] for s in code)

        # Relocations: VAs of 32-bit absolute pointers in the image.
        self.relocs = []
        if hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
            for blk in pe.DIRECTORY_ENTRY_BASERELOC:
                for e in blk.entries:
                    if e.type == 3:  # HIGHLOW
                        self.relocs.append(self.base + e.rva)

        # Imports: IAT slot VA -> "DLL!name"
        self.imports = {}
        for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = e.dll.decode().upper()
            for i in e.imports:
                n = i.name.decode() if i.name else f"#{i.ordinal}"
                self.imports[i.address] = f"{dll}!{n}"

        self.exports = {}
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                self.exports[self.base + s.address] = s.name.decode() if s.name else f"#{s.ordinal}"

    def is_code(self, va):
        return self.code_lo <= va < self.code_hi

    def in_image(self, va):
        return self.base <= va < self.base + self.size

    def u32(self, va):
        o = va - self.base
        return struct.unpack_from("<I", self.mem, o)[0]

    def bytes(self, va, n):
        o = va - self.base
        return bytes(self.mem[o : o + n])
