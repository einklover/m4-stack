#pragma once

// Device HTTPS origin and the phone-facing QR origin are the same production
// host. QEMU AES+GDMA (series-v3 patches 0010/0011) completes mbedtls hardware
// AES, so plugin-debug no longer rewrites this to a host TLS proxy.
#define M4_WEREAD_ORIGIN "https://weread.qq.com"
#define M4_WEREAD_PUBLIC_ORIGIN "https://weread.qq.com"
