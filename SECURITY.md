# Security Policy

## Supported versions

This project is pre-release software. Security fixes are applied to the latest
`main` branch; older commits and locally modified builds are not supported.

## Reporting a vulnerability

Do not disclose a suspected vulnerability, credential leak or unsafe
hardware-control behavior in a public issue.

Use GitHub's private vulnerability reporting or a private security advisory
for this repository when available. If that option is unavailable, contact
[@charlysound](https://github.com/charlysound) privately through GitHub before
sharing technical details.

Include the affected commit, operating system, interface and firmware version,
reproduction steps, expected impact and any proposed mitigation. Allow time for
the issue to be reproduced and addressed before public disclosure.

## Operational safety

oscmix changes live audio-interface state. Test untrusted builds with monitor
levels reduced, amplifiers muted where practical and a hardware mute available.
