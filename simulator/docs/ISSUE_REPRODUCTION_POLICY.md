# Issue reproduction and acceptance policy

Every Murphy field issue is classified by the lowest layer that can truthfully
reproduce it. A test is not labeled "reproduced" merely because a nearby mock
can be made to fail.

Statuses:

- `reproduced`: the failure mechanism is exercised by an automated regression.
- `modeled`: the relevant protocol/contract is modeled, but a real external or
  analog dependency prevents exact reproduction.
- `device_trace_required`: the simulator needs a captured field trace or
  physical measurement before its model can be calibrated honestly.
- `fixed_upstream_regression`: the firmware already has a focused regression;
  this repository links/checks it rather than duplicating application logic.

For every `reproduced` entry, the issue matrix must name an executable simulator
scenario, CTest target, or Python test. CI validates that no entry is left with
an empty test reference.

Analog EPD appearance, RF behavior, battery chemistry and signal-integrity
failures can be protocol-modeled but require real-device acceptance for final
physical claims.
