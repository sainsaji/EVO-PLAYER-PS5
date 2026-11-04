<!-- Thanks for contributing! Read CONTRIBUTING.md first: one pull request, one change.
     Describe only what is in this pull request, and say what you tested. -->

## Problem

<!-- What was wrong or missing from a viewer's point of view?
     Link the issue (Fixes #N). If there is none, this paragraph is the issue. -->

## What changed

<!-- Describe the resulting behaviour in a few sentences, not the functions you edited.
     For UI changes, include a tools/hui_preview.sh render or a console screenshot. -->

## How it was tested

<!-- Describe what you did and what happened.
     For console tests: model (PS5 / PS5 Pro), firmware, and the file or stream
     (container, codecs, where it comes from). Attach the report file or evo.log
     for playback changes.

     Host tests and PS5 builds are useful, but aren't console testing.
     If you couldn't test on a console, leave that box unchecked
     and say what still needs checking. Mark unrelated checks N/A. -->

- [ ] Compiles: `./scripts/build-evoplayer.sh`
- [ ] Host tests pass: `./tests/run_tests.sh`
- [ ] App module builds: `./scripts/package-app.sh --ffpfsc`
- [ ] Tested on a PS5, with steps and results described above
- [ ] Added or updated tests for changed behaviour, where applicable

## Scope

- [ ] This pull request does one thing. Unrelated fixes and features are in their own pull requests.
- [ ] It targets `main` and is rebased on it.
- [ ] It does not change the version (`VERSION`, `contentVersion`, `EVO_PLAYER_VERSION`), `CHANGELOG.md`,
      the in-app changelog or the generated `evo_rmlui_bundle_data.cpp`.
- [ ] New settings are new lines at the end of the settings file, covered by `tests/test_settings.c`.

## Submodule changes

<!-- Delete this section if no submodule pin changed. -->

- [ ] The new `third_party/ps5-homebrew-ui` pin is a commit already on the kit's `main` (link its pull request).

## AI tools

<!-- Did you use an AI coding tool? Which one, with which model, and roughly
     how much of the change did it write? "All of it" is a fine answer.
     Either way, you've read the result, checked this description against
     the diff, and run it yourself. -->
