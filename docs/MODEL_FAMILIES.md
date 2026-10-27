# Dataset model labels, not RF classification

The corpus harness associates filenames with product transmission families. It does not infer a
generation from IQ. These labels are not proof that every burst belongs to the named aircraft,
nor that a capture contains a DroneID broadcast. Sources checked 2026-10-26:

- Mini 2 / Mavic Air 2: O2; Air 2S: O3 — [DJI Air 2S support](https://www.dji.com/air-2s?from=overview&site=brandsite).
- Mini 3: **O2**, not O3 — [DJI Mini 3](https://www.dji.com/mini-3).
- Mini 3 Pro: O3 — [DJI Mini 3 Pro support](https://www.dji.com/mini-3-pro/faq).
- Mavic 3: O3+ — [DJI release announcement](https://www.dji.com/uk/newsroom/news/dji-mavic-3-release).
  Mavic 3C and Mavic 3 Pro use the operator's Classic/Pro mapping; ambiguous `mavic3S` is excluded.
- Mini 5 Pro: **O4+**, reported separately from O4 — [DJI specifications](https://www.dji.com/uk/mini-5-pro/specs?set_region=GB&site=brandsite).
- Matrice 300: OcuSync Enterprise, not simply O3 — [DJI announcement](https://www.dji.com/media-center/announcements/matrice-300-rtk-zenmuse-h20-series).
  The harness leaves its numeric generation unverified while retaining CRC/telemetry evidence.

Mini 4 Pro, Avata 2 and local Air 3S use the operator's O4 labels. Tampere Mavic Pro uses the
operator's O1 label. Other ambiguous models remain `unverified/other`, not guessed from packet content.
Explicit third-party controllers/aircraft are non-DJI controls, but are not necessarily RF-silent.

CRC-valid unsupported payloads are not v2 serial fields. In particular, the `49 4e 46 50` header
seen in some captures is retained as raw bytes, not labeled version 73 or decoded into a serial.
Neither an outer CRC pass nor its failure proves encryption. An O4/O4+ capture yielding CRC-valid
bits is not automatically a bug; telemetry parsing, identity and source attribution are separate tests.
