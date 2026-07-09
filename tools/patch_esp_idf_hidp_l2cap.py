#!/usr/bin/env python3
"""Patch ESP-IDF's public Classic L2CAP wrapper for HIDP basic mode.

ESP-IDF v5.3.2 implements esp_bt_l2cap_* through the JV/GAP OBEX path and
hard-codes ERTM. DualSense HIDP uses the standard HID control/interrupt PSMs
and expects Basic L2CAP, matching BTstack and Bluedroid's HID host.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


DEFAULT_IDF_PATH = Path(r"C:\tmp\esp-idf-v5.3.2")
BTC_L2CAP_REL = Path(
    "components/bt/host/bluedroid/btc/profile/std/l2cap/btc_l2cap.c"
)
PATCH_MARKER = "DS5_HIDP_PSM_CONTROL"


OBEX_BLOCK = """static const tL2CAP_ERTM_INFO obex_l2c_etm_opt =
{
    L2CAP_FCR_ERTM_MODE,            /* Mandatory for OBEX over l2cap */
    L2CAP_FCR_CHAN_OPT_ERTM,        /* Mandatory for OBEX over l2cap */
    OBX_USER_RX_POOL_ID,
    OBX_USER_TX_POOL_ID,
    OBX_FCR_RX_POOL_ID,
    OBX_FCR_TX_POOL_ID
};
"""

OBEX_BLOCK_PATCHED = OBEX_BLOCK + """
#define DS5_HIDP_PSM_CONTROL 0x0011
#define DS5_HIDP_PSM_INTERRUPT 0x0013

static const tL2CAP_ERTM_INFO hidp_l2c_basic_opt =
{
    L2CAP_FCR_BASIC_MODE,
    L2CAP_FCR_CHAN_OPT_BASIC,
    0,
    0,
    0,
    0
};

static bool btc_l2cap_is_hidp_psm(uint16_t psm)
{
    return psm == DS5_HIDP_PSM_CONTROL || psm == DS5_HIDP_PSM_INTERRUPT;
}
"""


START_SRV_ORIGINAL = """        /* Setup ETM settings */
        memset(&cfg, 0, sizeof(tL2CAP_CFG_INFO));
        cfg.fcr_present = TRUE;
        cfg.fcr = obex_l2c_fcr_opts_def;
        BTA_JvL2capStartServer(slot->security, slot->role, &obex_l2c_etm_opt, slot->psm,
                                    L2CAP_MAX_SDU_LENGTH, &cfg, (tBTA_JV_L2CAP_CBACK *)btc_l2cap_inter_cb, (void *)slot->id);
"""

START_SRV_PATCHED = """        /* Setup ETM settings */
        memset(&cfg, 0, sizeof(tL2CAP_CFG_INFO));
        const tL2CAP_ERTM_INFO *ertm_opt = &obex_l2c_etm_opt;
        if (btc_l2cap_is_hidp_psm(slot->psm)) {
            cfg.fcr_present = TRUE;
            memset(&cfg.fcr, 0, sizeof(cfg.fcr));
            cfg.fcr.mode = L2CAP_FCR_BASIC_MODE;
            ertm_opt = &hidp_l2c_basic_opt;
        } else {
            cfg.fcr_present = TRUE;
            cfg.fcr = obex_l2c_fcr_opts_def;
        }
        BTA_JvL2capStartServer(slot->security, slot->role, ertm_opt, slot->psm,
                                    L2CAP_MAX_SDU_LENGTH, &cfg, (tBTA_JV_L2CAP_CBACK *)btc_l2cap_inter_cb, (void *)slot->id);
"""


CONNECT_ORIGINAL = """        memset(&cfg, 0, sizeof(tL2CAP_CFG_INFO));
        cfg.fcr_present = TRUE;
        cfg.fcr = obex_l2c_fcr_opts_def;

        BTA_JvL2capConnect(slot->security, slot->role, &obex_l2c_etm_opt, slot->psm,
                            L2CAP_MAX_SDU_LENGTH, &cfg, slot->addr, (tBTA_JV_L2CAP_CBACK *)btc_l2cap_inter_cb, (void *)slot->id);
"""

CONNECT_PATCHED = """        memset(&cfg, 0, sizeof(tL2CAP_CFG_INFO));
        const tL2CAP_ERTM_INFO *ertm_opt = &obex_l2c_etm_opt;
        if (btc_l2cap_is_hidp_psm(slot->psm)) {
            cfg.fcr_present = TRUE;
            memset(&cfg.fcr, 0, sizeof(cfg.fcr));
            cfg.fcr.mode = L2CAP_FCR_BASIC_MODE;
            ertm_opt = &hidp_l2c_basic_opt;
        } else {
            cfg.fcr_present = TRUE;
            cfg.fcr = obex_l2c_fcr_opts_def;
        }

        BTA_JvL2capConnect(slot->security, slot->role, ertm_opt, slot->psm,
                            L2CAP_MAX_SDU_LENGTH, &cfg, slot->addr, (tBTA_JV_L2CAP_CBACK *)btc_l2cap_inter_cb, (void *)slot->id);
"""


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"expected one {label} block, found {count}")
    return text.replace(old, new, 1)


def patch_file(path: Path) -> bool:
    text = path.read_text(encoding="utf-8")
    if PATCH_MARKER in text:
        print(f"ESP-IDF HIDP L2CAP patch already present: {path}")
        return False

    text = replace_once(text, OBEX_BLOCK, OBEX_BLOCK_PATCHED, "OBEX ERTM")
    text = replace_once(text, START_SRV_ORIGINAL, START_SRV_PATCHED, "start server")
    text = replace_once(text, CONNECT_ORIGINAL, CONNECT_PATCHED, "connect")
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied ESP-IDF HIDP L2CAP basic-mode patch: {path}")
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=DEFAULT_IDF_PATH)
    args = parser.parse_args(argv)

    path = args.idf_path / BTC_L2CAP_REL
    if not path.is_file():
        print(f"missing ESP-IDF L2CAP source: {path}", file=sys.stderr)
        return 1
    try:
        patch_file(path)
    except Exception as exc:
        print(f"failed to patch ESP-IDF L2CAP source: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
