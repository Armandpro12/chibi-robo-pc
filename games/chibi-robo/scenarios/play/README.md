# Progression scenarios

Long scenarios that play a new game forward with hand-written inputs (milestone 8, "playable game"). They check that
the game progresses without a crash on replayed inputs; they have no recorded reference.

They live in `play/<ID>/` on purpose: `tools/ab.py` (through `tools/scenario.py`) only runs `scenarios/*.scn` and
`scenarios/<ID>/*.scn`, so these several-minute runs never slow down the A/B regression bench. Run one on demand:

    py -3 tools/run.py chibi-robo --version GGTE01 --exe <exe> --audio off \
        --scenario games/chibi-robo/scenarios/play/GGTE01/first-task.scn --out <folder>

Add `--trace dvd` to list the files read (stage changes) and `-- --hle-trace` to trace the REL modules: each link
prints `gcrt hle: OSLink module <id> (<name>): ...` and each unlink `gcrt hle: OSUnlink module <name> -> 1` in
`stderr.log`; the final call counts are in `stdout.log` (the table in `stderr.log` is printed at startup, before any
call). A scenario with an `after <name>` line starts from the user folder (saves) of that scenario's run:
`--user-seed <run of name>/user`.
Each file of `play/<ID>/` is written for that version only (frame numbers differ between versions); the same name in
two version folders is the same logical route.

Scenarios (same name in `GGTE01/` and `GGTP01/`):

- `first-task`: new game, birthday cutscene, the first task (the flower on the party table), Chibi-Copter tutorial.
- `party`: `first-task`, then the rose given to the girl and the end of the party, the first change of stage
  (stage 14 to stage 5, the Chibi-House) and the first save (file 1). No REL module is loaded during it (stages
  14 and 5 have none).
- `house-exit` (`after party`): Load Game, file 1, out of the Chibi-House to the living room (stage 7: first link
  of `stage07.rel`), back into the Chibi-House (unlink), out again (second link).
- `staff-credits`: loads any save into the credits movie with `--stage staff-credit` (see the header of the file).
- `basement-movie`: loads the `new-game` save into the basement with `--stage basement`, walks to the socket of the
  big robot and plugs in: the basement movie (see the header of the file; the PAL path after the cutscene differs).

Hints for writing a route: the stick directions are relative to the camera, which turns while Chibi-Robo walks;
A with the stick pushed jumps; in these runs, A next to a "!" icon did nothing until B had put the Chibi-Copter away (that the equipped
copter is the cause is only a hypothesis).
In the Chibi-House, A next to the charger starts the ranking and save dialogue: no A there once Chibi-Robo is free.
