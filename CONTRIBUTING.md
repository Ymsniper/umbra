# Contributing

Pull requests and issues are welcome.

## What is useful

Most of this repository is not specific to one game, and that is the part worth
improving:

* the X11 overlay, click-through and window tracking
* skeleton composition from the mesh hierarchy
* the visibility check built on the engine's render timestamps
* the kernel module: memory reads and pointer-level input
* build, packaging and documentation

Ports to other Unreal titles are welcome too. Everything above is engine-level
rather than title-level.

## What is not here

The offset derivation tooling is deliberately not in this repository. That means
a game update will make `offsets.cfg` stale and there is no script here to
regenerate it.

I may push updated offsets myself when a patch breaks them, but that is if and
when I feel like it, not a commitment. Treat any working `offsets.cfg` here as a
snapshot rather than something maintained on a schedule.

Please do not open issues asking for the derivation method, and do not open
issues reporting that the offsets stopped working after a patch. That is
expected and documented in the README.

## Before opening a pull request

Build cleanly and run it:

```bash
./build.sh clean
```

It should finish with no warnings. If you touched the kernel module, build that
too, and say which kernel and toolchain you tested on.

Keep changes focused. One thing per pull request is much easier to review than
several.

## Style

Roughly match the surrounding code and that is enough. Nothing here is strict
enough to turn a pull request away over, and a working fix formatted differently
is better than no fix.

One preference rather than a rule: comments tend to read better when they say
what the code does and why it is not obvious, instead of how it came to be
written. If yours carry a few notes about what used to be broken, that is fine.
I will tidy them rather than send the pull request back.

## Reporting a bug

Include:

* distribution and kernel version (`uname -r`)
* session type (`echo $XDG_SESSION_TYPE`) and whether XWayland is in use
* whether the kernel module was loaded
* the relevant output, especially the `[offsets]`, `[settings]` and `[x11]` lines

"It does not work" without any of that is difficult to act on.

## License

Contributions are accepted under the GPL-2.0, the same licence as the rest of
the project. You keep copyright on what you write.
