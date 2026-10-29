# Contributing

[Español](CONTRIBUTING.md) · **English**

## Conventions

- C++20 + Qt 6. Format with `.clang-format`. Code and identifiers are in English.
- Commits follow Conventional Commits in English (`feat:`, `fix:`, `chore:`, `docs:`, `test:`).
- Every behavior change comes with a Qt Test. Write the test first.
- Never hardcode secrets; read them from the environment or a `.env` file that is not committed.

## Legal boundaries

- Never commit Wallpaper Engine assets, Workshop items or Anima Engine files, including as test fixtures. Tests build synthetic fixtures in temporary directories.
- Never copy code from other wallpaper engines; implement behaviour and file formats from public documentation and the files themselves.
- Workshop subscriptions go through the user's Steam client (`steam://` URLs). Never impersonate a Steam App ID.

## Contributor agreement (dual licensing)

Murasaki is dual-licensed (GPL-3.0-or-later and a commercial license, see `LICENSING.en.md`). By submitting a contribution you agree that:

1. you wrote it yourself, or have the right to submit it;
2. it is licensed to the project under the GPL-3.0-or-later; and
3. you grant the copyright holder (uriel2121ger-art) a perpetual, worldwide, royalty-free, irrevocable license to use, modify and relicense it under any terms, including the commercial license.

Add a `Signed-off-by:` line (`git commit -s`) to confirm this. Do not copy code from other projects unless it is under a permissive license and listed in `docs/third-party.md`; never copy code from GPL projects (read them for behaviour only).
