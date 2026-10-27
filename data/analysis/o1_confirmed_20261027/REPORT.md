# Local corpus validation

All windows are bounded prefixes, not whole-archive or whole-dataset coverage.
Model/family names are dataset/operator labels, not independently identified PHY generations.

| Dataset | Model | Family label | Windows checked | CRC-valid unique payloads | V2 serial parsed | RF-active windows | Errors |
|---|---|---|---:|---:|---:|---:|---:|
| Tampere | DJI_mavic_pro_2G | O1 | 1 | 1 | 1 | 1 | 0 |

## O4 accuracy limitation

The current backend emits `unknown_rf`, not O4 identities. Its O4 identification accuracy is **not established**.
The following confusion counts deliberately test the incorrect proxy `any RF activity => O4` against explicit model labels.
They quantify why the energy detector is insufficient; they are not an O4 classifier accuracy claim.
No model was trained/tuned on this evaluation and no per-burst ground truth is available.

```json
{
  "o4_labeled_windows": 0,
  "non_o4_labeled_windows": 1,
  "o4_plus_windows_excluded": 0,
  "activity_true_positive": 0,
  "activity_false_negative": 0,
  "activity_false_positive": 1,
  "activity_true_negative": 0,
  "activity_recall": null,
  "activity_false_positive_rate": 1.0,
  "activity_precision": 0.0,
  "o4_classifier_accuracy": null,
  "reason": "No O4 classifier: all identities abstain. RF activity is not O4 detection."
}
```

Decode success means outer CRC-24 passed. Missing CRCs do not prove encryption, incompatibility, or drone absence.
Control/video RF captures may contain no DroneID broadcast. Check individual logs and sampled durations before interpreting failures.
