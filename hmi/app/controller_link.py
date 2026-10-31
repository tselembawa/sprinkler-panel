"""How the HMI reaches the Main Controller.

Today: SimController in the same process.
Later: SerialController, which speaks the framed UART protocol (COBS + CRC-16)
to the real microcontroller and exposes exactly the same methods:

    upload_config(points, user) -> {"ok", "errors", "version", "crc"}
    config_info()               -> {"version", "crc", "points"}
    get_status()                -> dict (relays, LEDs, counts, last_event_seq, ...)
    get_points()                -> list of runtime states
    get_events(from_seq, max)   -> list of events
    cmd_ack(user), cmd_reset(user), cmd_disable(point_id, disabled, user)

The sim_* methods only exist on the simulator.
"""

import os

from .sim_controller import CommandError, SimController

__all__ = ["CommandError", "make_controller"]


def make_controller(start_seq: int = 0):
    kind = os.environ.get("SPRINKLER_CONTROLLER", "sim").lower()
    if kind == "sim":
        return SimController(start_seq=start_seq)
    raise RuntimeError(
        f"Controller type '{kind}' is not implemented yet. Use SPRINKLER_CONTROLLER=sim."
    )
