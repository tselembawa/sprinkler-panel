"""
Whole system on one Linux PC (or WSL), without hardware:

    module simulator  <-pty->  controller firmware (host_app)  <-pty->  HMI (SPRINKLER_CONTROLLER=serial:PORT)

    gcc ... -o /tmp/host_app   (see firmware/README.md)
    python tools/module_sim/dev_bench.py /tmp/host_app
    # in a second terminal, with the port it prints:
    SPRINKLER_CONTROLLER=serial:/dev/pts/7 uvicorn app.main:app --app-dir hmi --port 8080

Keys 1-4 / s / v / r of the module simulator work in this terminal (module L1-M02).
"""
import os
import pty
import subprocess
import sys
import threading
import time
import tty

sys.path.insert(0, os.path.dirname(__file__))
import module_sim as ms  # noqa: E402


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    host_app = sys.argv[1]
    rs_master, rs_slave = pty.openpty()
    pi_master, pi_slave = pty.openpty()
    for fd in (rs_master, rs_slave, pi_master, pi_slave):
        tty.setraw(fd)
    proc = subprocess.Popen([host_app, str(rs_master)], pass_fds=[rs_master],
                            stdin=pi_master, stdout=pi_master)
    port = os.ttyname(pi_slave)
    print(f"\nController (host_app) is running. HMI port:  SPRINKLER_CONTROLLER=serial:{port}\n", flush=True)
    keys = sys.stdin.isatty() and "--no-keys" not in sys.argv
    sim = threading.Thread(target=ms.run, args=(os.ttyname(rs_slave), [2]), kwargs=dict(keys=keys), daemon=True)
    sim.start()
    try:
        while sim.is_alive() and proc.poll() is None:
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
