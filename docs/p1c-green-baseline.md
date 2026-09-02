# P1C GREEN baseline

## Current stacked PR

- PR: #60
- Issue: #59
- Base: runtime/p1b-file-transfer-service-ownership
- Base SHA: a6dd2c3ce93d807dcf482e19c9aa5d38fbd4e61a
- Head: runtime/p1c-esp-http-server-contracts
- Head SHA: 960e947a1e3ce91dc39c27a615ab219457b46337

## Scope

The P1C migration keeps M4FileTransferService as the owner of file-transfer HTTP lifecycle while moving transport ownership to ESP-IDF esp_http_server contracts.

## Verification checkpoint

- esp_http_server contract wiring is present.
- Production/plugin-debug build verification was added in the latest CI commit.
- Remaining closeout should attach exact Actions evidence from the PR head.
