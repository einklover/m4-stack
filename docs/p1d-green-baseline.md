# P1D GREEN baseline

## Branch

`runtime/p1d-navigation-supervisor`

## Exact verified head

`c2b6604e`

## Simulator-only verification

P1D is accepted using host contracts and QEMU/CI evidence.

Verified gates:

- contracts: GREEN
- murphy_m4 firmware build: GREEN
- plugin-debug QEMU build: GREEN

Workflow evidence:

- gate run: `33627504136`
- regression/contracts run: `33627506636`

## Scope boundary

This closeout does not require device flashing or real-device validation.

P1D invariant:

> Accepting Home/Back is a UI/supervisor decision and is not contingent on synchronous network-server destruction.

Navigation supervisor wiring remains unchanged after verification.
