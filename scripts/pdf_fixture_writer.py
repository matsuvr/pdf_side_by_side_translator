"""Small original PDF writer shared by development fixture generators."""


def objects_pdf(path, objects, trailer_extra=b"", transform=None):
    data = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for index, obj in enumerate(objects, 1):
        if transform:
            obj = transform(index, obj)
        offsets.append(len(data))
        data.extend(f"{index} 0 obj\n".encode() + obj + b"\nendobj\n")
    xref = len(data)
    data.extend(f"xref\n0 {len(offsets)}\n0000000000 65535 f \n".encode())
    for at in offsets[1:]:
        data.extend(f"{at:010d} 00000 n \n".encode())
    data.extend(f"trailer\n<< /Size {len(offsets)} /Root 1 0 R ".encode() + trailer_extra + f">>\nstartxref\n{xref}\n%%EOF\n".encode())
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def stream(body, extra=b""):
    return f"<< /Length {len(body)} ".encode() + extra + b">>\nstream\n" + body + b"\nendstream"
