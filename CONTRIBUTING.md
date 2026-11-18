# Contributing

Thank you for looking. pacreact is small, open and meant to be changed. Contributions, forks and
alternatives are all welcome.

The rules for every pac tool are in [pacommon/CONTRIBUTING.md](pacommon/CONTRIBUTING.md), and what the
tools must be is in [pacommon/PRINCIPLES.md](pacommon/PRINCIPLES.md). In short:

- One fix or one feature per change, with a test, and the README, the example `pacreact.conf` and `docs/`
  kept true. A change that breaks a principle says why in `docs/decisions.md`.
- `make` has no warnings and `make test` passes. Say on which distribution and compositor you tried it.
- AI may be used if a person reads, understands and tests the change, and the change says so
  ([AGENTS.md](AGENTS.md) tells an AI agent how to work here).
- Translations go through gettext (`po/`); a machine translation is marked `fuzzy` until a native speaker has read it.
- Nothing is sent anywhere, nothing is hidden: principles 44 and 56 do not bend.
