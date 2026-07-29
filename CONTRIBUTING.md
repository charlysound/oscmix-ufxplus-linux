# Contributing

Contributions that improve reliability, hardware compatibility, documentation
or the GTK4 user experience are welcome.

## Before opening a pull request

1. Search the existing issues and pull requests.
2. Fork the repository and create a focused branch.
3. Keep user-facing application text, source comments and GitHub documentation
   in English.
4. Avoid fixed ALSA client numbers, USB controller paths or other
   machine-specific assumptions.
5. Test hardware-control changes with monitor levels reduced.

## Build and validation

On Debian or Ubuntu:

```sh
sudo apt install build-essential libasound2-dev pkg-config \
  libgtk-4-dev libavahi-client-dev
make -j2 oscmix alsaseqio gtk4
make -C gtk4 check
```

Describe any physical-hardware validation in the pull request, including:

- interface model and firmware version;
- Linux distribution and kernel version;
- USB Class Compliant mode;
- the controls and reconnection scenarios tested.

Hardware-dependent changes should degrade safely when a register is not
available. UI-only tests do not replace verification at a low monitoring
level.

## Pull requests

Keep each pull request limited to one coherent change. Explain:

- what changed and why;
- the root cause for a bug fix;
- user-visible behavior;
- commands and hardware used for validation;
- known limitations or follow-up work.

Do not include generated binaries, personal configuration, device serial
numbers, credentials or unrelated formatting changes.

By contributing, you agree that your contribution may be distributed under
the repository's existing [license](LICENSE). Retain upstream copyright and
license notices.
