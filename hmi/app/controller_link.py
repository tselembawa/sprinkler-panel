"""How the HMI reaches the Main Controller.

    SPRINKLER_CONTROLLER=sim            SimController in the same process (default, development)
    SPRINKLER_CONTROLLER=serial[:PORT]  SerialController: the real controller over the framed
                                        UART protocol (docs/pi-link.md, COBS + CRC-16)

Both expose the same methods:

    upload_config(points, user) -> {"ok", "errors", "version", "crc"}
    config_info()               -> {"version", "crc", "points"}
    get_status()                -> dict (relays, LEDs, counts, last_event_seq, ...)
    get_points()                -> list of runtime states
    get_events(from_seq, max)   -> list of events
    cmd_ack, cmd_reset, cmd_silence, cmd_service, cmd_disable, set_time

The sim_* methods only exist on the simulator; dev_set_key only on the serial prototype
(until the key switch is wired to the controller). A SerialController raises
ControllerUnavailable while the controller does not answer.
"""

import os

from .sim_controller import CommandError, ControllerUnavailable, SimController

__all__ = ["CommandError", "ControllerUnavailable", "make_controller"]


def make_controller(start_seq: int = 0, store=None):
    kind = os.environ.get("SPRINKLER_CONTROLLER", "sim")
    name, _, port = kind.partition(":")
    name = name.lower()
    if name == "sim":
        return SimController(start_seq=start_seq)
    if name == "serial":
        from .serial_controller import SerialController, SerialLink, open_serial

        link = SerialLink(lambda: open_serial(port or None))
        return SerialController(link, store=store, start_seq=start_seq)
    raise RuntimeError(f"Unknown controller type '{kind}'. Use SPRINKLER_CONTROLLER=sim or serial[:PORT].")
