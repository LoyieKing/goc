# Security Policy

## Supported versions

`goc` is an **experimental / research** compiler. There is no stable supported
release line yet. Security fixes land on `main` when practical.

## Reporting a vulnerability

Please **do not** open a public GitHub issue for security-sensitive reports.

Email the maintainer:

- Loyie King — `724180662@163.com`

Include:

1. A clear description of the issue and impact
2. Steps to reproduce (minimal `.c` / IR if possible)
3. Affected commit hash or tag

You should receive an acknowledgement within a few days. Please give a
reasonable window before public disclosure.

## Scope notes

- Untrusted C input compiled with `goc` may crash the compiler or produce
  incorrect code; treat the toolchain as a research prototype.
- Do not report “I can make the experimental compiler crash on weird IR”
  unless it demonstrates a concrete privilege, integrity, or supply-chain
  concern in a documented build path.
