# Frozen O4 candidate profile: external dataset evaluation

Frozen o4_structure_v2 on preselected files/archive members absent from the development evaluation. 0.64-second prefixes; no tuning on these results. Files may share sessions/devices with development, so this is file-held-out evidence, not session-held-out identity accuracy. Recording labels are not event truth.

| Family label | Captures | With candidates | Candidate / total windows | Seconds |
|---|---:|---:|---:|---:|
| O2 | 4 | 0 | 0 / 160 | 2.560 |
| O3 | 8 | 0 | 0 / 320 | 5.120 |
| O3+ | 10 | 0 | 0 / 400 | 6.400 |
| O4 | 8 | 7 | 202 / 320 | 5.120 |
| O4+ | 3 | 0 | 0 / 120 | 1.920 |
| non-DJI | 2 | 0 | 0 / 80 | 1.280 |

Errors: 0. No IQ downloaded or source files modified.

No sensitivity/precision claim from recording-level labels. No per-file normalization or favorable subband selection.

| Case | Model | Group | Center MHz | Candidate / total windows |
|---|---|---|---:|---:|
| 000-195eff0062 | mavic3 | dataset1-single_drone_states | 5745.0 | 0 / 40 |
| 001-0f4190c4d9 | mavic3C | dataset1-single_drone_states | 5745.0 | 0 / 40 |
| 002-3459b802e2 | mavicAir2 | dataset1-single_drone_states | 5745.0 | 0 / 40 |
| 003-7176069210 | mavicAir2s | dataset1-single_drone_states | 5745.0 | 0 / 40 |
| 004-09dcfe6044 | mini3pro | dataset1-single_drone_states | 5745.0 | 0 / 40 |
| 005-d94768cca0 | mini4PRO | dataset1-single_drone_states | 5745.0 | 36 / 40 |
| 006-bdd921d827 | mini5PRO | dataset1-single_drone_states | 5745.0 | 0 / 40 |
| 007-25f689a4ed | mavic3 | dataset3-single_drone_hover | 5745.0 | 0 / 40 |
| 008-419dc9a45a | mavic3C | dataset3-single_drone_hover | 5745.0 | 0 / 40 |
| 009-66f20d4d84 | mavicAir2 | dataset3-single_drone_hover | 5745.0 | 0 / 40 |
| 010-d0186a48c7 | mavicAir2s | dataset3-single_drone_hover | 5745.0 | 0 / 40 |
| 011-bcefe6344e | mini3pro | dataset3-single_drone_hover | 5745.0 | 0 / 40 |
| 012-0f6f7f9d6c | mini4PRO | dataset3-single_drone_hover | 5745.0 | 36 / 40 |
| 013-da94023daa | mini5PRO | dataset3-single_drone_hover | 5745.0 | 0 / 40 |
| 014-7928c566d1 | mavic3 | dataset4-single_drone_dual_frequency | 5745.0 | 0 / 40 |
| 015-f84aba3ea6 | mavic3C | dataset4-single_drone_dual_frequency | 5745.0 | 0 / 40 |
| 016-da01f63862 | mavicAir2 | dataset4-single_drone_dual_frequency | 5745.0 | 0 / 40 |
| 017-f2899610e5 | mavicAir2s | dataset4-single_drone_dual_frequency | 5745.0 | 0 / 40 |
| 018-14cbf0ad86 | mini3pro | dataset4-single_drone_dual_frequency | 5745.0 | 0 / 40 |
| 019-4a525b5e83 | mini4PRO | dataset4-single_drone_dual_frequency | 5745.0 | 2 / 40 |
| 020-907a5accc7 | mavic3 | dataset6-wifi_mixed | 2437.0 | 0 / 40 |
| 021-3b5f6a1c72 | mavic3C | dataset6-wifi_mixed | 2437.0 | 0 / 40 |
| 022-e8e6fdb543 | mavicAir2 | dataset6-wifi_mixed | 2437.0 | 0 / 40 |
| 023-887aed6a1a | mavicAir2s | dataset6-wifi_mixed | 2437.0 | 0 / 40 |
| 024-5fd254fa05 | mini3pro | dataset6-wifi_mixed | 2437.0 | 0 / 40 |
| 025-5ac0b26d06 | mini4PRO | dataset6-wifi_mixed | 2437.0 | 27 / 40 |
| 026-e8e439d0ce | mini5PRO | dataset6-wifi_mixed | 2437.0 | 0 / 40 |
| 027-7d04e6c49f | DJI AVATA2 | preselected_middle_last_members | 5760.0 | 0 / 40 |
| 028-7758eeaaa2 | DJI AVATA2 | preselected_middle_last_members | 5760.0 | 29 / 40 |
| 029-6d7abf3b15 | DJI MINI4 PRO | preselected_middle_last_members | 2450.0 | 36 / 40 |
| 030-1021c25225 | DJI MINI4 PRO | preselected_middle_last_members | 2450.0 | 36 / 40 |
| 031-f1a4271927 | DJI MAVIC3 PRO | preselected_middle_last_members | 5800.0 | 0 / 40 |
| 032-3018ac5539 | DJI MAVIC3 PRO | preselected_middle_last_members | 5800.0 | 0 / 40 |
| 033-b391b5147d | FRSKY X14 | preselected_middle_last_members | 2440.0 | 0 / 40 |
| 034-46fe339c05 | FRSKY X14 | preselected_middle_last_members | 2440.0 | 0 / 40 |
