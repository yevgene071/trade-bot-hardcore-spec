#!/usr/bin/env python3
"""Embed a file as a C header (portable replacement for xxd -i)."""
import sys

def main() -> None:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_file> <output_header>", file=sys.stderr)
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2]

    with open(input_file, "rb") as f:
        data = f.read() + b'\x00'  # null-terminator to prevent strlen overrun

    size = len(data)

    lines: list[str] = []
    lines.append(f"unsigned char dashboard_html[{size}] = {{")

    for i, byte in enumerate(data):
        if i % 12 == 0:
            lines.append("  ")
        lines.append(f"0x{byte:02x}, ")
        if i % 12 == 11:
            lines.append("\n")

    lines.append(f"\n}};\n")
    lines.append(f"unsigned int dashboard_html_len = {size};\n")
    lines.append("\n")
    lines.append(
        "static const char* kDashboardHtml = "
        'reinterpret_cast<const char*>(dashboard_html);\n'
    )
    lines.append(
        f"static const unsigned int kDashboardHtmlLen = dashboard_html_len;\n"
    )

    with open(output_file, "w") as f:
        f.write("".join(lines))


if __name__ == "__main__":
    main()
