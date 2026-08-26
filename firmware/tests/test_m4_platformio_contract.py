#!/usr/bin/env python3
"""Static contract for portable Murphy M4 PlatformIO defaults."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
PLATFORMIO = ROOT / "firmware" / "platformio.ini"
LEGADO_HEADER = ROOT / "firmware" / "src" / "apps" / "providers" / "M4LegadoBridge.h"
LEGADO_SOURCE = ROOT / "firmware" / "src" / "apps" / "providers" / "M4LegadoBridge.cpp"
ENDPOINT_ACTIVITY = ROOT / "firmware" / "src" / "activities" / "apps" / "NativeProviderEndpointActivity.cpp"

PRIVATE_IPV4 = re.compile(
    r"(?<![0-9.])(?:10\.[0-9]{1,3}(?:\.[0-9]{1,3}){2}|"
    r"172\.(?:1[6-9]|2[0-9]|3[0-1])\.[0-9]{1,3}(?:\.[0-9]{1,3})|"
    r"192\.168\.[0-9]{1,3}\.[0-9]{1,3})(?![0-9.])"
)


def section(config: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^\[{re.escape(name)}\]\n(?P<body>.*?)(?=^\[|\Z)",
        config,
    )
    assert match, f"missing PlatformIO section: {name}"
    return match.group("body")


def test_m4_platformio_defaults_are_portable() -> None:
    config = PLATFORMIO.read_text(encoding="utf-8")

    platformio = section(config, "platformio")
    default_envs = re.search(r"(?m)^default_envs\s*=\s*(\S+)\s*$", platformio)
    assert default_envs and default_envs.group(1) == "murphy_m4"

    env_names = set(re.findall(r"(?m)^\[env:([^]]+)\]$", config))
    assert {"murphy_m4", "murphy_m4_qemu", "murphy_m4_qemu_plugin"} <= env_names

    production = section(config, "env:murphy_m4")
    qemu = section(config, "env:murphy_m4_qemu")
    qemu_plugin = section(config, "env:murphy_m4_qemu_plugin")
    assert "M4_QEMU_BUILD=1" not in production
    assert "M4_QEMU_PLUGIN_DEBUG=1" not in production
    assert "M4_QEMU_BUILD=1" in qemu
    assert "M4_QEMU_PLUGIN_DEBUG=1" in qemu_plugin

    for env_name in ("murphy_m4", "murphy_m4_qemu_plugin"):
        body = section(config, f"env:{env_name}")
        assert not PRIVATE_IPV4.search(body), f"private endpoint in {env_name} defaults"

    legado_header = LEGADO_HEADER.read_text(encoding="utf-8")
    legado_source = LEGADO_SOURCE.read_text(encoding="utf-8")
    endpoint_activity = ENDPOINT_ACTIVITY.read_text(encoding="utf-8")
    load_saved_body = endpoint_activity.split(
        "void NativeProviderEndpointActivity::loadSavedEndpoint()", 1
    )[1].split("void NativeProviderEndpointActivity::editField", 1)[0]

    assert not PRIVATE_IPV4.search(legado_header)
    assert not PRIVATE_IPV4.search(legado_source)
    assert not PRIVATE_IPV4.search(load_saved_body)
    assert "kDefaultBase" not in legado_header
    assert "kDefaultBase" not in legado_source
    assert "kDefaultBase" not in load_saved_body
    assert re.search(r"std::string baseUrl\(\)\s*\{.*?return gBase;", legado_source, re.DOTALL)
    assert "probeBase(kDefaultBase" not in legado_source


if __name__ == "__main__":
    test_m4_platformio_defaults_are_portable()
    print("m4 PlatformIO config contract: PASS")
