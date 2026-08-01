#!/usr/bin/env python3
"""Compare official Castle Peak firmware without constructing a BIOS image."""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


IMAGE_SIZE = 16 << 20
MAGIC_OFFSET = 0x20000
MAGIC = bytes.fromhex("aa55aa55")
EXPECTED = {
    "asrock": {
        "sha256": "f8a52cbba6b8000a8b300de019cbee0e034daf0d1919f34cf44767cd31708a01",
        "agesa": "CastlePeakPI-SP3r3-1.0.0.3",
    },
    "gigabyte-f4": {
        "sha256": "dad8a44f8120e9a4a3b4a0ef95c51e5aabafec782bef45523f4425916b41375a",
        "agesa": "CastlePeakPI-SP3r3-1.0.0.5",
    },
    "gigabyte-f7g": {
        "sha256": "ccd471a8bbbba8e9b2b869dbc3f8be31d359307f01c489b65973bef1a878e002",
        "agesa": "CastlePeakPI-SP3r3-1.0.0.F",
    },
    "asus-2402": {
        "sha256": "9afe781688a786c657874cf269fc748c5f2ad8cdeca3027cd2fbca14ab7d462d",
        "agesa": "CastlePeakPI-SP3r3-1.0.0.F",
    },
}

FILE_LINE = re.compile(
    r"^\s*File\s+\|\s*([^|]+?)\s*\|\s*([0-9A-F]+|N/A)\s*\|"
    r"\s*([0-9A-F]+)\s*\|\s*([0-9A-F]+)\s*\|\s*-+\s*"
    r"([0-9A-F-]{36})(?:\s*\|\s*(.*?))?\s*$"
)
GUID_LINE = re.compile(r"^File GUID: ([0-9A-F-]{36})$", re.MULTILINE)
TEXT_LINE = re.compile(r"^Text: (.*)$", re.MULTILINE)
EXEC_LINE = re.compile(r"^Subtype: (?:PE32|TE) image$", re.MULTILINE)
AGESA_PATTERN = re.compile(rb"CastlePeakPI-SP3r3-[0-9A-Fa-f.]+")
AMD_NAME_HINTS = (
    "Amd",
    "Apcb",
    "Apob",
    "Ccx",
    "Fabric",
    "Fch",
    "Mem",
    "Nbio",
    "Psp",
    "Ras",
    "Smu",
    "Soc",
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_image(label, path):
    expected = EXPECTED[label]
    if path.stat().st_size != IMAGE_SIZE:
        raise ValueError(f"{label}: expected a 16 MiB raw image")
    actual_hash = sha256(path)
    if actual_hash != expected["sha256"]:
        raise ValueError(f"{label}: unexpected SHA-256 {actual_hash}")
    data = path.read_bytes()
    if data[MAGIC_OFFSET : MAGIC_OFFSET + len(MAGIC)] != MAGIC:
        raise ValueError(f"{label}: missing aa55aa55 marker at 0x20000")
    versions = sorted({item.decode("ascii") for item in AGESA_PATTERN.findall(data)})
    if versions != [expected["agesa"]]:
        raise ValueError(f"{label}: unexpected AGESA strings {versions}")
    return {"path": str(path), "size": len(data), "sha256": actual_hash, "agesa": versions[0]}


def run_tool(arguments, cwd, log_path, stdout_path=None):
    result = subprocess.run(arguments, cwd=cwd, capture_output=True, check=False)
    log_path.write_bytes(result.stderr)
    if stdout_path is not None:
        stdout_path.write_bytes(result.stdout)
    elif result.stdout:
        with log_path.open("ab") as stream:
            stream.write(result.stdout)
    if result.returncode:
        command = " ".join(str(item) for item in arguments)
        raise RuntimeError(f"command failed ({result.returncode}): {command}")


def parse_ffs_report(path):
    rows = []
    for line in path.read_text(errors="replace").splitlines():
        match = FILE_LINE.match(line)
        if not match:
            continue
        subtype, base, size, crc32, guid, name = match.groups()
        rows.append(
            {
                "guid": guid,
                "name": (name or "").strip(),
                "subtype": subtype.strip(),
                "base": base,
                "size": int(size, 16),
                "crc32": crc32,
            }
        )
    return rows


def hash_executables(dump_root):
    file_nodes = {}
    info_files = list(dump_root.rglob("info.txt"))
    for info in info_files:
        text = info.read_text(errors="replace")
        match = GUID_LINE.search(text)
        if match:
            name = TEXT_LINE.search(text)
            file_nodes[info.parent] = {
                "guid": match.group(1),
                "name": name.group(1) if name else "",
                "executables": [],
            }

    for info in info_files:
        text = info.read_text(errors="replace")
        if not EXEC_LINE.search(text):
            continue
        body = info.with_name("body.bin")
        if not body.exists():
            continue
        parent = info.parent
        while parent != dump_root and parent not in file_nodes:
            parent = parent.parent
        if parent in file_nodes:
            file_nodes[parent]["executables"].append(sha256(body))

    result = {}
    for item in file_nodes.values():
        if not item["executables"]:
            continue
        result.setdefault(item["guid"], []).append(
            {"name": item["name"], "sha256": sorted(item["executables"])}
        )
    return result


def compare_ffs(left, right):
    left_map = {item["guid"]: item for item in left}
    right_map = {item["guid"]: item for item in right}
    common = set(left_map) & set(right_map)
    identical = {guid for guid in common if left_map[guid]["crc32"] == right_map[guid]["crc32"]}
    changed = common - identical
    return {
        "left_files": len(left),
        "right_files": len(right),
        "common_guids": len(common),
        "identical_crc32": len(identical),
        "changed_crc32": len(changed),
        "left_only_guids": len(set(left_map) - set(right_map)),
        "right_only_guids": len(set(right_map) - set(left_map)),
        "changed_amd_modules": sorted(
            {
                left_map[guid]["name"] or right_map[guid]["name"]
                for guid in changed
                if (left_map[guid]["name"] or right_map[guid]["name"]).startswith(AMD_NAME_HINTS)
            }
        ),
    }


def compare_executables(left, right):
    common = set(left) & set(right)
    identical = {
        guid
        for guid in common
        if sorted(item["sha256"] for item in left[guid])
        == sorted(item["sha256"] for item in right[guid])
    }
    return {
        "common_guids": len(common),
        "identical_executables": len(identical),
        "changed_executables": len(common - identical),
        "left_only_guids": len(set(left) - set(right)),
        "right_only_guids": len(set(right) - set(left)),
    }


def psp_entry(psp, directory, section_type):
    for item in psp:
        if item["directory"] != directory:
            continue
        for entry in item["entries"]:
            if entry["sectionType"] == section_type:
                return entry
    raise ValueError(f"missing PSP directory {directory} entry {section_type}")


def psp_summary(psp):
    wanted = (
        "PSP_FW_BOOT_LOADER~0x1",
        "PSP_FW_RECOVERY_BOOT_LOADER~0x3",
        "SMU_OFFCHIP_FW~0x8",
        "PSP_FW_TRUSTED_OS~0x2",
        "PSP_BOOT_TIME_TRUSTLETS~0xc",
        "DRIVER_ENTRIES~0x28",
        "MP5_FW~0x2a",
        "PREMIUM_CHIPSET_MP1_FW~0x2f",
    )
    result = {}
    for section_type in wanted:
        matches = []
        for directory in psp:
            for entry in directory["entries"]:
                if entry["sectionType"] == section_type:
                    matches.append(entry)
        if matches:
            entry = matches[-1]
            result[section_type] = {
                "version": entry.get("version", ""),
                "md5": entry.get("md5", ""),
                "size": entry["size"],
                "info": entry.get("info", []),
            }
    apcb = psp_entry(psp, 3, "APCB~0x60")
    result["APCB~0x60"] = {
        "version": apcb.get("version", ""),
        "md5": apcb.get("md5", ""),
        "size": apcb["size"],
        "info": apcb.get("info", []),
    }
    return result


def render_markdown(result):
    lines = [
        "# TRX40D8-2N2T AGESA donor analysis",
        "",
        "This report is generated from hash-pinned official images. The tool deliberately does not create a flashable BIOS image.",
        "",
        "## Inputs",
        "",
        "| Image | AGESA | SHA-256 |",
        "|---|---|---|",
    ]
    for label, item in result["inputs"].items():
        lines.append(f"| {label} | `{item['agesa']}` | `{item['sha256']}` |")

    lines.extend(["", "## Firmware-volume comparisons", ""])
    for name, item in result["ffs_comparisons"].items():
        lines.append(
            f"- `{name}`: {item['changed_crc32']} of {item['common_guids']} common FFS GUIDs changed; "
            f"{item['identical_crc32']} retained the same file CRC."
        )
    if result.get("executable_comparisons"):
        lines.extend(["", "## Decompressed executable comparisons", ""])
        for name, item in result["executable_comparisons"].items():
            lines.append(
                f"- `{name}`: {item['changed_executables']} of {item['common_guids']} common executable GUIDs changed; "
                f"{item['identical_executables']} are byte-identical after extraction."
            )

    lines.extend(
        [
            "",
            "## PSP and board-data versions",
            "",
            "| Image | PSP boot loader | SMU firmware | APCB fingerprint |",
            "|---|---|---|---|",
        ]
    )
    for label, image in result["images"].items():
        psp = image["psp"]
        lines.append(
            f"| {label} | `{psp['PSP_FW_BOOT_LOADER~0x1']['version']}` | "
            f"`{psp['SMU_OFFCHIP_FW~0x8']['version']}` | `{psp['APCB~0x60']['md5']}` |"
        )

    lines.extend(
        [
            "",
            "## Safety decision",
            "",
            f"Candidate image permitted: **{str(result['candidate_permitted']).lower()}**",
            "",
        ]
    )
    for reason in result["stop_reasons"]:
        lines.append(f"- {reason}")
    lines.extend(
        [
            "",
            "The ASRock APCB remains board-specific and must not be replaced by donor data. Its compatibility with the AGESA 1.0.0.F module set is not established by these binaries.",
            "",
        ]
    )
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asrock", required=True, type=Path)
    parser.add_argument("--gigabyte-f4", required=True, type=Path)
    parser.add_argument("--gigabyte-f7g", required=True, type=Path)
    parser.add_argument("--asus-2402", required=True, type=Path)
    parser.add_argument("--uefiextract", required=True, type=Path)
    parser.add_argument("--psptool", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--skip-deep", action="store_true", help="skip decompressed executable hashing")
    args = parser.parse_args()

    images = {
        "asrock": args.asrock.resolve(),
        "gigabyte-f4": args.gigabyte_f4.resolve(),
        "gigabyte-f7g": args.gigabyte_f7g.resolve(),
        "asus-2402": args.asus_2402.resolve(),
    }
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)

    result = {"inputs": {}, "tools": {}, "images": {}}
    for tool_name, tool_path in (("uefiextract", args.uefiextract), ("psptool", args.psptool)):
        resolved = tool_path.resolve()
        if not resolved.is_file():
            parser.error(f"missing {tool_name}: {resolved}")
        result["tools"][tool_name] = str(resolved)

    for label, image in images.items():
        result["inputs"][label] = validate_image(label, image)
        image_dir = output / label
        image_dir.mkdir()
        local_image = image_dir / "firmware.bin"
        shutil.copyfile(image, local_image)

        run_tool(
            [str(args.uefiextract.resolve()), "firmware.bin", "report"],
            image_dir,
            image_dir / "uefiextract-report.log",
        )
        report_path = image_dir / "firmware.bin.report.txt"
        ffs = parse_ffs_report(report_path)
        if not ffs:
            raise RuntimeError(f"{label}: UEFIExtract produced no FFS records")

        psp_path = image_dir / "psp-entries.json"
        run_tool(
            [str(args.psptool.resolve()), "-E", "-j", "firmware.bin"],
            image_dir,
            image_dir / "psptool.log",
            psp_path,
        )
        psp = json.loads(psp_path.read_text())
        image_result = {"ffs": ffs, "psp": psp_summary(psp)}

        if not args.skip_deep:
            run_tool(
                [str(args.uefiextract.resolve()), "firmware.bin", "dump"],
                image_dir,
                image_dir / "uefiextract-dump.log",
            )
            image_result["executables"] = hash_executables(image_dir / "firmware.bin.dump")
        result["images"][label] = image_result

    pairs = (
        ("asrock_to_gigabyte_f4", "asrock", "gigabyte-f4"),
        ("asrock_to_gigabyte_f7g", "asrock", "gigabyte-f7g"),
        ("gigabyte_f4_to_f7g", "gigabyte-f4", "gigabyte-f7g"),
        ("gigabyte_f7g_to_asus_2402", "gigabyte-f7g", "asus-2402"),
    )
    result["ffs_comparisons"] = {
        name: compare_ffs(result["images"][left]["ffs"], result["images"][right]["ffs"])
        for name, left, right in pairs
    }
    if not args.skip_deep:
        result["executable_comparisons"] = {
            name: compare_executables(
                result["images"][left]["executables"], result["images"][right]["executables"]
            )
            for name, left, right in pairs
        }

    result["candidate_permitted"] = False
    result["stop_reasons"] = [
        "The same-board Gigabyte AGESA 1.0.0.5 to 1.0.0.F update changes hundreds of common UEFI modules, so AGESA is not an isolated replaceable payload.",
        "AGESA 1.0.0.F is coupled to a newer signed PSP bootloader, trusted OS, trustlets, driver bundle, SMU firmware, MP5 firmware, and chipset firmware.",
        "All four boards have different APCB hashes; donor APCB data encodes board-specific memory and topology configuration and cannot replace the ASRock APCB.",
        "No vendor build manifest or matching CastlePeakPI source package proves a closed module set that combines ASRock board data with the donor firmware stack.",
    ]

    (output / "summary.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    (output / "REPORT.md").write_text(render_markdown(result))
    print(output / "REPORT.md")
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
