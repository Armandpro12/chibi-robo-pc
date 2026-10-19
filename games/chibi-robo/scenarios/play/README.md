# Progression scenarios

Long scenarios that play a new game forward with hand-written inputs (milestone 8, "playable game"). They check that
the game progresses without a crash on replayed inputs; they have no recorded reference.

They live in `play/<ID>/` on purpose: `tools/ab.py` (through `tools/scenario.py`) only runs `scenarios/*.scn` and
`scenarios/<ID>/*.scn`, so these several-minute runs never slow down the A/B regression bench. Run one on demand:

    py -3 tools/run.py chibi-robo --version GGTE01 --exe <exe> --audio off \
        --scenario games/chibi-robo/scenarios/play/GGTE01/first-task.scn --out <folder>

Add `--trace dvd` to list the files read (stage changes) and `-- --hle-trace` to count the `OSLink` calls (REL modules).
Each file of `play/<ID>/` is written for that version only (frame numbers differ between versions); the same name in
two version folders is the same logical route.

Scenarios (same name in `GGTE01/` and `GGTP01/`):

- `first-task`: new game, birthday cutscene, the first task (the flower on the party table), Chibi-Copter tutorial.
- `party`: `first-task`, then the rose given to Jenny and the end of the party, up to the first change of stage
  (stage 14 to stage 5, the Chibi-House). No REL module is loaded during it (stages 14 and 5 have none).

Hints for writing a route: the stick directions are relative to the camera, which turns while Chibi-Robo walks;
A with the stick pushed jumps; in these runs, A next to a "!" icon did nothing while the Chibi-Copter was equipped and
talked once B had put it away.
