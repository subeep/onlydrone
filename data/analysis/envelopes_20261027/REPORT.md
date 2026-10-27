# Saved-packet envelope audit

Rechecks saved outer-CRC-valid hex; no new RF coverage or IQ downloads.
Deduplicated by exact 176-byte transport payload across supplied reports.

Unique payloads: **22**. Inner CRC16 status: `{'OK': 22}`.

| Dataset | Model label | Family label | Envelope | Logical bytes | Unique payloads |
|---|---|---|---|---:|---:|
| DroneRFa | mavic3 | O3+ | infp | 131 | 2 |
| DroneRFa | mavicAir2s | O3 | v2 | 91 | 2 |
| DroneRFa | mini5PRO | O4+ | infp | 138 | 1 |
| KULeuven | MavicRC1 | unverified/other | v2 | 91 | 1 |
| KULeuven | matriceRC | unverified/other | v2 | 79 | 1 |
| KULeuven | mini2RC | O2 | v2 | 91 | 1 |
| RFUAV | DJI MAVIC3 PRO | O3+ | v3 | 112 | 2 |
| RFUAV | DJI MINI3 | O2 | auxiliary_11 | 144 | 1 |
| RFUAV | DJI MINI3 | O2 | v2 | 91 | 1 |
| RUB-reference | mavic_air_2 | O2 | v2 | 91 | 1 |
| RUB-reference | mini2 | O2 | v2 | 91 | 8 |
| Tampere | DJI_mavic_pro_2G | O1 | v2 | 91 | 1 |

## Interpretation

Logical length is first byte + 3; inner CRC16 uses reflected polynomial 0x8408, initial 0x3692, zero residue over the logical record.
CRC checks detect corruption; they do not authenticate identity, validate telemetry truth, or identify an OcuSync generation.
`v3`, `infp`, `cryp` and auxiliary records are format diagnostics, not decoded serial/GPS telemetry.
The same INFP marker occurs in O3+ and O4+ labeled captures: it cannot establish strict O4 identity.
Family/model labels come from dataset metadata; source labels are not per-burst ground truth. Counts across rows need not sum to the unique total.
No CRYP-positive local fixture is claimed unless listed in the table. CRYP recognition also has synthetic regression coverage.

Sources: [packet envelope research](https://github.com/luyii-code-1/dji-ocusync-droneid-research),
[v2 protocol paper](https://www.ndss-symposium.org/wp-content/uploads/2023/02/ndss2023_f217_paper.pdf).
Only the envelope/checksum observations were adopted; unsupported schemas and broader research claims were not assumed.
