# Physics bench (story 10-13)

Scores RAV's auto detection against references measured from motion-capture markers, not hand
tags. Each corpus is fetched on demand, converted to RAV detection csvs, given references
computed from the markers' geometry, and run through `tests/trigger_bench`. The report gives recall,
precision and signed timing error per kind, per condition and per technique.

```
uv run tools/physics_bench/run.py                 # Kyokushin, 12 subjects (about 180 clips)
uv run tools/physics_bench/run.py --subjects=2    # a quick run (30 clips)
uv run tools/physics_bench/run.py --subjects=all  # every subject
uv run tools/physics_bench/run.py --no-fetch      # cached trials only
uv run tools/physics_bench/selftest.py            # synthetic checks of the reference rules
```

You need `uv` and nothing else. The script brings numpy, c3d, cmake, ninja and zig. It builds
`trigger_bench` with the system C++ compiler when one is on PATH, else with zig.

| Where | What |
|---|---|
| `$RAV_PHYSICS_CACHE` (default `~/.cache/rav-physics`) | downloaded trials, the zip listing, the bench build |
| `fixtures/_physics/<corpus>/` | `<clip>.csv`, `<clip>.ref.tsv`, `clips.tsv`, `doubtful.tsv` |
| `fixtures/_physics/<corpus>/_bench/` | `bench.txt` (the whole bench output), `last.tsv`, `matches.tsv`, `review.json` |
| `fixtures/_physics/report.txt` | the summary printed at the end |

The 72-clip game bench (`tools/bench/run.sh`) is separate. It reads only the top level of
`fixtures/`, so the physics clips never mix into it.

A second run downloads nothing and reconverts nothing. Its "changes against the previous run"
section stays empty unless the detection changed.

Costs, measured on WSL with the default 12 subjects:

- **Cache:** about 1.6 GB. T01 of every subject is about 4.8 GB zipped.
- **First run:** about 17 minutes.
- **Re-run:** about 14 minutes, mostly `trigger_bench` on 180 clips. Every run reads all the C3D
  files again to recompute the references; only the csvs are kept when nothing changed.

## Kyokushin karate (first corpus)

37 athletes, Vicon at 250 Hz, Plug-In Gait. Techniques (the paper's data records): S01 Gyaku-Zuki
(reverse punch), S02 Mae-Geri (front kick), S03 Mawashi-Geri gedan (low roundhouse kick), S04
Mawashi-Geri jodan (high roundhouse kick) and S05 Ushiro-Mawashi-Geri (spinning back kick).
Conditions: E01 in the air, E02 on a shield, E03 on an opponent.

- **Trials.** By default the bench takes T01 of the first 12 subjects that have all 15 trials
  (S01-S05 × E01-E03). E04 (the defender's view) is skipped because it is the same take as the
  partner's E03 (the file sizes are identical).
- **Clips.** Clip id `kyo-<subject>-<Sxx>-<Exx>-<Txx>`. The PiG joint centres become mixamorig
  bones, in metres, Y up, at 240 Hz.
- **Kind.** S01 gives `punch`, part `hand`. S02-S05 give `kick`, part `any`. The side is the
  striking limb.
- **Effectors.** Hand: `HNO` (the PiG hand origin, ahead of the knuckles) and `FIN`. Foot: `TOE`,
  `HEE`, `ANK` and the PiG foot and toe origins. The effector's speed is the mean of its markers'
  speeds, and its reach is the mean of its markers' reaches. A centroid would jump when a marker
  drops out.
- **Timing grid.** References sit on the 250 Hz grid of the C3D, while the csv is 240 Hz. This
  adds about ±2 ms of quantisation to the timing error.

References, one per blow, status `confirmed`, source `physics`:

| Condition | Reference |
|---|---|
| shield | The first frame where the effector comes within 20 mm of the shield quad (`Tarcza1-4`), after an approach of at least 3 m/s (hand) or 4 m/s (foot) in the previous 60 ms. Contacts less than 300 ms apart merge. The markers sink into the pad, so crossing 20 mm marks the arrival. The closest approach can come 130 ms later, with the foot resting on the pad. |
| opponent | For each effector speed peak (at least 3 or 4 m/s, the strongest within ±300 ms), the closest approach to any of the partner's 39 markers between 50 ms before and 100 ms after the peak. Under 80 mm, the reference is the first frame within closest + 30 mm. |
| air | For each speed peak of at least 4 m/s (hand) or 6 m/s (foot) with the reach rising, the reference is the maximum reach (effector to shoulder or hip) within 200 ms after the peak. This is full extension, the moment a Hit marker aims at. |

The script marks some references `draft` and lists each of them, with its reason, in
`doubtful.tsv`. These are the only cases that go to Antho:

- **maybe no contact (closest N mm):** on an opponent, the closest approach is 50-80 mm.
- **count N, usual M:** one side's blow count differs from the most common count (mode) of the same
  technique × condition, counted over the sides that have at least one blow.
- **marker gap:** more than 20 % of the frames within ±100 ms are missing for the effector's raw
  markers, or for the shield.
- **no blow:** the clip has no reference at all. It is listed once, with no time.

To settle a draft, edit its line in `<clip>.ref.tsv`: set the status to `confirmed` or `deleted`
and the source to `antho`. Antho's lines (source `antho`, any status) survive every run. The
script drops any computed reference of the same kind and side within 0.15 s of one of them.

If a trial cannot be fetched, the script resolves the signed URL again and tries 3 times, then
skips the trial. A trial that cannot be read is skipped too. Skipped trials are listed on the
run's `skipped:` line, and any clips they produced in an earlier run stay in the folder.

## Data hygiene and licences

Third-party data is never committed to either repo and never shipped. Trials are read from the
remote zip with HTTP range requests into the cache, outside the repo. Converted clips stay in
`fixtures/`, which git ignores.

- **Kyokushin karate**, CC0. Szczęsna A., Błaszczyszyn M., Pawlyta M., "Optical motion capture
  dataset of selected techniques in beginner and advanced Kyokushin karate athletes",
  *Scientific Data* 8, 13 (2021), doi:10.1038/s41597-021-00801-5. Data: figshare file 24601187
  (`Karate.zip`).
