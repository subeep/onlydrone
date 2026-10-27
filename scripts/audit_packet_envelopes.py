#!/usr/bin/env python3
"""Inspect saved decoder payloads without rereading or downloading IQ."""
import argparse
from collections import Counter
import json
from pathlib import Path

from validate_corpus import packet_record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    packets = {}
    for report in args.reports:
        for row in json.loads((report / "results.json").read_text()):
            for packet in row.get("unique_payloads", []):
                payload = packet["payload_hex"]
                record = packets.setdefault(payload, dict(**packet_record(payload), sources=[]))
                source = dict(dataset=row["dataset"], model=row["model"], family=row["family"], path=row["path"])
                if source not in record["sources"]:
                    record["sources"].append(source)
    if not packets:
        parser.error("reports contain no decoded payloads")
    records = sorted(packets.values(), key=lambda record: record["payload_hex"])
    models = {}
    for record in records:
        for source in record["sources"]:
            key = (source["dataset"], source["model"], source["family"], record["packet_format"], record["logical_bytes"])
            models.setdefault(key, set()).add(record["payload_hex"])
    counts = Counter(record["inner_crc16"] for record in records)
    lines = ["# Saved-packet envelope audit", "", "Rechecks saved outer-CRC-valid hex; no new RF coverage or IQ downloads.",
             "Deduplicated by exact 176-byte transport payload across supplied reports.", "",
             f"Unique payloads: **{len(records)}**. Inner CRC16 status: `{dict(counts)}`.", "",
             "| Dataset | Model label | Family label | Envelope | Logical bytes | Unique payloads |",
             "|---|---|---|---|---:|---:|"]
    for (dataset, model, family, form, length), payloads in sorted(models.items()):
        lines.append(f"| {dataset} | {model} | {family} | {form} | {length} | {len(payloads)} |")
    lines += ["", "## Interpretation", "",
              "Logical length is first byte + 3; inner CRC16 uses reflected polynomial 0x8408, initial 0x3692, zero residue over the logical record.",
              "CRC checks detect corruption; they do not authenticate identity, validate telemetry truth, or identify an OcuSync generation.",
              "`v3`, `infp`, `cryp` and auxiliary records are format diagnostics, not decoded serial/GPS telemetry.",
              "The same INFP marker occurs in O3+ and O4+ labeled captures: it cannot establish strict O4 identity.",
              "Family/model labels come from dataset metadata; source labels are not per-burst ground truth. Counts across rows need not sum to the unique total.",
              "No CRYP-positive local fixture is claimed unless listed in the table. CRYP recognition also has synthetic regression coverage.", "",
              "Sources: [packet envelope research](https://github.com/luyii-code-1/dji-ocusync-droneid-research),",
              "[v2 protocol paper](https://www.ndss-symposium.org/wp-content/uploads/2023/02/ndss2023_f217_paper.pdf).",
              "Only the envelope/checksum observations were adopted; unsupported schemas and broader research claims were not assumed."]
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "REPORT.md").write_text("\n".join(lines) + "\n")
    (args.output / "packets.json").write_text(json.dumps(records, indent=2) + "\n")
    print(json.dumps(dict(unique_payloads=len(records), inner_crc16=dict(counts), output=str(args.output))))
    return 0 if counts["OK"] == len(records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
