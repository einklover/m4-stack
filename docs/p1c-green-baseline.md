# P1C GREEN baseline

## Current stacked PR

- PR: #60
- Issue: #59
- Base: runtime/p1b-file-transfer-service-ownership
- Base SHA: a6dd2c3ce93d807dcf482e19c9aa5d38fbd4e61a
- Head: runtime/p1c-esp-http-server-contracts
- Head SHA: 84c14badd89eb4565f59a22b400a6712772541cd

## Scope

The P1C migration keeps M4FileTransferService as the owner of file-transfer HTTP lifecycle while moving transport ownership to ESP-IDF esp_http_server contracts.

## Verification checkpoint

- esp_http_server contract wiring is present.
- Production/plugin-debug build verification was added in the latest CI commit.
- Exact GREEN Actions evidence:
  - Actions run: 33594742670
  - contracts: SUCCESS
  - firmware-build: SUCCESS
