#!/usr/bin/env python3
"""Finish AES+GDMA so a TLS record cannot wedge the guest CPU.

Login works (tiny HTTPS). Shelf sync hangs the vCPU because:

1. OUT/IN LINK is a 20-bit field on silicon, but ESP-IDF writes the full
   virtual address into the 32-bit register. Reconstructing only
   0x3FC80000|(addr&0xfffff) turns a PSRAM/I-bus pointer into a DRAM
   miss. GDMA read fails, AES never writes STATE=DONE, mbedtls busy-waits
   forever, UART poll never runs.

2. AES trigger runs DMA+gcrypt synchronously and, on any GDMA error,
   returns without DONE/IRQ. Same busy-wait.

Use the software-written high bits when present, and always retire an AES
DMA trigger with STATE=DONE (+IRQ if enabled).
"""
from __future__ import annotations

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one upstream match, found {count}")
    return text.replace(old, new, 1)


def patch_gdma(root: Path) -> None:
    path = root / "hw/dma/esp_gdma.c"
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        """static void esp_gdma_get_restart_buffer(ESPGdmaState *s, uint32_t chan, uint32_t dir, uint32_t* out)
{""",
        """static uint32_t esp_gdma_link_addr(uint32_t link)
{
    /* Hardware ADDR is 20 bits. Software writes the full pointer plus START
     * into the same word. Prefer those high bits so PSRAM / I-bus descriptors
     * are not forced into the 0x3FC8_0000 DRAM window. */
    const uint32_t control = R_GDMA_OUT_LINK_START_MASK |
                             R_GDMA_OUT_LINK_RESTART_MASK |
                             R_GDMA_OUT_LINK_STOP_MASK |
                             R_GDMA_OUT_LINK_PARK_MASK;
    const uint32_t raw = link & ~control;
    const uint32_t high = raw & ~R_GDMA_OUT_LINK_ADDR_MASK;
    if (high) {
        return raw;
    }
    return ((ESP_GDMA_RAM_ADDR >> 20) << 20) | (raw & R_GDMA_OUT_LINK_ADDR_MASK);
}

static void esp_gdma_get_restart_buffer(ESPGdmaState *s, uint32_t chan, uint32_t dir, uint32_t* out)
{""",
        "insert link_addr helper",
    )
    text = replace_once(
        text,
        """    uint32_t out_addr = ((ESP_GDMA_RAM_ADDR >> 20) << 20) | FIELD_EX32(state->link, GDMA_OUT_LINK, ADDR);""",
        """    uint32_t out_addr = esp_gdma_link_addr(state->link);""",
        "read_channel full addr",
    )
    text = replace_once(
        text,
        """    uint32_t in_addr = ((ESP_GDMA_RAM_ADDR >> 20) << 20) | FIELD_EX32(state->link, GDMA_IN_LINK, ADDR);""",
        """    uint32_t in_addr = esp_gdma_link_addr(state->link);""",
        "write_channel full addr",
    )
    path.write_text(text, encoding="utf-8")


def patch_aes(root: Path) -> None:
    path = root / "hw/misc/esp_aes.c"
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        """    if ( !esp_gdma_get_channel_periph(s->gdma, GDMA_AES, ESP_GDMA_OUT_IDX, &gdma_out_idx) ||
         !esp_gdma_get_channel_periph(s->gdma, GDMA_AES, ESP_GDMA_IN_IDX, &gdma_in_idx) ) {
        warn_report("[AES] GDMA requested but no properly configured channel found");
        goto close_exit;
    }""",
        """    if ( !esp_gdma_get_channel_periph(s->gdma, GDMA_AES, ESP_GDMA_OUT_IDX, &gdma_out_idx) ||
         !esp_gdma_get_channel_periph(s->gdma, GDMA_AES, ESP_GDMA_IN_IDX, &gdma_in_idx) ) {
        warn_report("[AES] GDMA requested but no properly configured channel found");
        goto done_exit;
    }""",
        "aes missing channel still completes",
    )
    text = replace_once(
        text,
        """    if ( !esp_gdma_read_channel(s->gdma, gdma_out_idx, buffer, buf_size) ) {
        warn_report("[AES] Error reading from GDMA buffer");
        goto close_exit;
    }""",
        """    if ( !esp_gdma_read_channel(s->gdma, gdma_out_idx, buffer, buf_size) ) {
        warn_report("[AES] Error reading from GDMA buffer");
        goto done_exit;
    }""",
        "aes read fail still completes",
    )
    text = replace_once(
        text,
        """    if ( !esp_gdma_write_channel(s->gdma, gdma_in_idx, buffer, buf_size) ) {
        warn_report("[AES] Error writing to GDMA buffer");
        goto close_exit;
    }

    s->state_reg = ESP_AES_DONE;

    if (s->int_ena_reg) {
        qemu_irq_raise(s->irq);
    }

close_exit:
    gcry_cipher_close(ghandle);
}""",
        """    if ( !esp_gdma_write_channel(s->gdma, gdma_in_idx, buffer, buf_size) ) {
        warn_report("[AES] Error writing to GDMA buffer");
        goto done_exit;
    }

done_exit:
    /* mbedtls polls AES_STATE. Never leave a DMA trigger without DONE or the
     * guest CPU spins and UART/input die. */
    s->state_reg = ESP_AES_DONE;
    if (s->int_ena_reg) {
        qemu_irq_raise(s->irq);
    }

close_exit:
    gcry_cipher_close(ghandle);
}""",
        "aes always DONE",
    )
    path.write_text(text, encoding="utf-8")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} QEMU_SOURCE", file=sys.stderr)
        return 2
    root = Path(argv[1]).resolve()
    patch_gdma(root)
    patch_aes(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
