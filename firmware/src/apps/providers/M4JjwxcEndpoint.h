#pragma once

// Production firmware talks to the HTTPS CDN. QEMU plugin-debug still uses the
// android HTTP origin because getFullPage is chunked / has no Content-Length:
// esp_http_client waits for EOF after the first window is already parsed.
// Discovery now cancels at maxRows, but the HTTP origin keeps the QEMU path
// from also paying a TLS stall on the leftover body.
//
// m.jjwxc.net 301-redirects HTTP→HTTPS, so VIP WAP chapters stay unavailable
// under QEMU; free android-API chapters work.
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
#define M4_JJWXC_APP_CDN "http://android.jjwxc.net"
#define M4_JJWXC_WAP_BASE "http://m.jjwxc.net"
#define M4_JJWXC_MY_BASE "http://my.jjwxc.net"
#else
#define M4_JJWXC_APP_CDN "https://app-cdn.jjwxc.net"
#define M4_JJWXC_WAP_BASE "https://m.jjwxc.net"
#define M4_JJWXC_MY_BASE "https://my.jjwxc.net"
#endif
