#!/usr/bin/env python3
"""Filament colour and material from the Spoolman spool saved for each tool.

Without a printer: save_variables is a dict, Moonraker is a socket pair or a
local HTTP server, and lookups run on the calling thread instead of a worker.

    python tests/test_spoolman.py      # or: pytest tests/
"""

import http.server
import json
import os
import socket
import sys
import threading
import time

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_ROOT, "klippy_extras"))

import knomi_serial as k  # noqa: E402


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, wanted {want!r}")


def spool(color_hex=None, material="PLA", multi=None):
    filament = {"material": material}
    if color_hex is not None:
        filament["color_hex"] = color_hex
    if multi is not None:
        filament["multi_color_hexes"] = multi
    return {"id": 9, "filament": filament}


# --- reading saved variables and spool records ------------------------------


def test_the_variable_is_the_one_mainsail_and_fluidd_save():
    check("T0", k.spool_variable("0"), "t0__spool_id")
    check("named tool", k.spool_variable("A"), "ta__spool_id")


def test_only_a_positive_id_is_a_spool():
    for value, want in ((9, 9), ("11", 11), (" 4 ", 4), (None, 0), (0, 0),
                        (-3, 0), ("", 0), ("None", 0), (True, 0), (2.5, 0)):
        check(repr(value), k.parse_spool_id(value), want)


def test_colour_and_material_come_from_the_filament():
    check("plain", k.spool_filament(spool("FF8800")), (0xFF8800, "PLA"))
    check("hash", k.spool_filament(spool("#00ff7f")), (0x00FF7F, "PLA"))
    check("alpha dropped", k.spool_filament(spool("11223344")), (0x112233, "PLA"))


def test_a_multicolour_filament_shows_its_first_colour():
    got = k.spool_filament(spool(multi="AA0000,00BB00", material="Silk PLA"))
    check("multi", got, (0xAA0000, "Silk PLA"))


def test_missing_or_bad_fields_are_none_rather_than_a_guess():
    check("no colour", k.spool_filament(spool()), (None, "PLA"))
    check("bad colour", k.spool_filament(spool("orange", material="")),
          (None, None))
    check("no filament", k.spool_filament({"id": 9}), (None, None))
    check("not a record", k.spool_filament(None), (None, None))
    long_name = k.spool_filament(spool("FFFFFF", material="X" * 40))[1]
    check("material fits the wire", len(long_name), 15)


# --- talking to Moonraker ---------------------------------------------------


def serve_socket(replies):
    """A socket pair whose far end reads two requests and sends `replies`."""
    near, far = socket.socketpair()
    seen = []

    def run():
        pending = b""
        while pending.count(k._ETX) < 2:
            chunk = far.recv(4096)
            if not chunk:
                return
            pending += chunk
        seen.extend(json.loads(m) for m in pending.split(k._ETX) if m)
        for reply in replies:
            far.sendall(reply)

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return near, far, seen, thread


def frame(message):
    return json.dumps(message).encode() + k._ETX


def test_a_socket_call_skips_notifications_and_split_frames():
    answer = frame({"jsonrpc": "2.0", "result": spool("FF8800"), "id": 2})
    near, far, seen, thread = serve_socket([
        frame({"jsonrpc": "2.0", "method": "notify_proc_stat_update",
               "params": [{}]}),
        frame({"jsonrpc": "2.0", "result": {"connection_id": 1}, "id": 1}),
        answer[:10],
        answer[10:],
    ])
    try:
        got = k.jsonrpc_call(near, "server.spoolman.proxy",
                             {"request_method": "GET", "path": "/v1/spool/9"},
                             time.monotonic() + 2)
    finally:
        thread.join(2)
        near.close()
        far.close()
    check("result", got, spool("FF8800"))
    check("identified first", seen[0]["method"], "server.connection.identify")
    check("then asked", seen[1]["params"]["path"], "/v1/spool/9")


def test_a_socket_error_reply_is_raised():
    near, far, _, thread = serve_socket([
        frame({"jsonrpc": "2.0", "id": 2,
               "error": {"code": -32601, "message": "Method not found"}}),
    ])
    try:
        k.jsonrpc_call(near, "server.spoolman.proxy", {}, time.monotonic() + 2)
    except k.MoonrakerError as e:
        if "Method not found" not in str(e):
            raise AssertionError(f"error should carry Moonraker's text: {e}") from None
    else:
        raise AssertionError("an error reply was taken as a result")
    finally:
        thread.join(2)
        near.close()
        far.close()


def test_a_silent_moonraker_times_out():
    near, far = socket.socketpair()
    try:
        k.jsonrpc_call(near, "server.spoolman.proxy", {}, time.monotonic() + 0.2)
    except (TimeoutError, socket.timeout):
        pass
    else:
        raise AssertionError("waited for an answer that never came")
    finally:
        near.close()
        far.close()


def ask_http(result, spool_id=11):
    """moonraker_spool against a one-shot HTTP Moonraker answering `result`."""
    asked = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            body = self.rfile.read(int(self.headers["Content-Length"]))
            asked.append((self.path, json.loads(body)))
            data = json.dumps({"result": result}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()
    try:
        url = f"http://127.0.0.1:{server.server_address[1]}/"
        return k.moonraker_spool(url, spool_id, timeout=2), asked
    finally:
        thread.join(2)
        server.server_close()


def refused(result, phrase):
    try:
        got = ask_http(result)
    except k.MoonrakerError as e:
        if phrase not in str(e):
            raise AssertionError(f"error should say {phrase!r}: {e}") from None
        return
    raise AssertionError(f"accepted {result!r} as a spool: {got!r}")


def test_an_http_endpoint_asks_the_proxy_for_a_v2_response():
    got, asked = ask_http({"response": spool("123456"), "error": None})
    check("unwrapped", got, spool("123456"))
    check("asked", asked, [("/server/spoolman/proxy",
                            {"request_method": "GET", "path": "/v1/spool/11",
                             "use_v2_response": True})])


def test_an_older_moonraker_answering_v1_still_works():
    got, _ = ask_http(spool("123456"))
    check("passed through", got, spool("123456"))


def test_spoolman_errors_are_raised_not_shown_as_no_colour():
    refused({"response": None,
             "error": {"status_code": 404, "message": "No spool with ID 11 found."}},
            "No spool with ID 11 found.")
    # The deprecated reply passes Spoolman's error body straight through.
    refused({"message": "No spool with ID 11 found."}, "did not return spool 11")


# --- following spools on the cluster ----------------------------------------


class FakeSaveVariables:
    def __init__(self, **variables):
        self.allVariables = dict(variables)


class FakeReactor:
    def register_async_callback(self, callback):
        callback(100.0)


class FakeDevice:
    def __init__(self, screen_name, config_tool, spoolman=True):
        self.screen_name = screen_name
        self.name = f"knomi_serial {screen_name}"
        self.config_tool = config_tool
        self.config_spoolman = spoolman


class Spoolman:
    """Answers lookups from a dict of spool records; None means unreachable."""

    def __init__(self, **records):
        self.records = {int(key[1:]): value for key, value in records.items()}
        self.asked = []
        self.down = False

    def __call__(self, endpoint, spool_id):
        self.asked.append(spool_id)
        if self.down:
            raise ConnectionRefusedError("Moonraker is not up")
        return self.records[spool_id]


def cluster(save_variables, spoolman, *devices):
    c = k.KnomiCluster.__new__(k.KnomiCluster)
    c.devices = list(devices)
    c.tools = {}
    c.was_printing = False
    c.reactor = FakeReactor()
    c._init_spoolman()
    c.save_variables = save_variables
    c._fetch_spool = spoolman
    c._start_lookup = lambda work: work()
    return c


def row(spoolman=None, **variables):
    spoolman = spoolman or Spoolman(s9=spool("FF8800", "PLA"),
                                    s11=spool("0000FF", "PETG"))
    c = cluster(FakeSaveVariables(**variables), spoolman,
                FakeDevice("T0_knomi", "0"), FakeDevice("T1_knomi", "1"))
    return c, spoolman


def shown(c, screen):
    state = c.tool_state(screen)
    return state.spool_id, state.color, state.type


def test_the_first_tick_looks_up_every_saved_spool():
    c, spoolman = row(t0__spool_id=9, t1__spool_id=11)
    c._track_spools(1.0)
    check("T0", shown(c, "T0_knomi"), (9, 0xFF8800, "PLA"))
    check("T1", shown(c, "T1_knomi"), (11, 0x0000FF, "PETG"))
    check("asked", spoolman.asked, [9, 11])


def test_an_unchanged_spool_is_not_asked_about_again():
    c, spoolman = row(t0__spool_id=9)
    for tick in range(5):
        c._track_spools(float(tick))
    check("asked once", spoolman.asked, [9])


def test_a_changed_spool_is_looked_up():
    c, spoolman = row(t0__spool_id=9)
    c._track_spools(1.0)
    c.save_variables.allVariables["t0__spool_id"] = 11
    c._track_spools(2.0)
    check("T0", shown(c, "T0_knomi"), (11, 0x0000FF, "PETG"))
    check("asked", spoolman.asked, [9, 11])


def test_a_print_start_looks_every_spool_up_again():
    c, spoolman = row(t0__spool_id=9)
    c._track_spools(1.0)
    spoolman.records[9] = spool("00FF00", "ASA")
    c._track_job("printing")
    c._track_spools(2.0)
    c._track_job("printing")
    c._track_spools(3.0)
    check("refreshed", shown(c, "T0_knomi"), (9, 0x00FF00, "ASA"))
    check("once per job", spoolman.asked, [9, 9])


def test_a_cleared_spool_goes_back_to_unknown():
    c, _ = row(t0__spool_id=9)
    c._track_spools(1.0)
    c.save_variables.allVariables["t0__spool_id"] = None
    c._track_spools(2.0)
    check("cleared", shown(c, "T0_knomi"), (0, 0, ""))


def test_a_tool_that_never_had_a_spool_keeps_knomi_tool_colour():
    c, spoolman = row(t0__spool_id=9)
    c.tool_state("T1_knomi").color = 0x123456
    c.tool_state("T1_knomi").type = "TPU"
    c._track_spools(1.0)
    c._track_spools(2.0)
    check("untouched", shown(c, "T1_knomi"), (0, 0x123456, "TPU"))
    check("never asked", spoolman.asked, [9])


def test_a_lookup_replaces_a_manual_colour():
    c, _ = row(t0__spool_id=9)
    c._track_spools(1.0)
    c.tool_state("T0_knomi").color = 0x123456
    c._track_spools(2.0)
    check("manual holds until the next lookup", shown(c, "T0_knomi")[1], 0x123456)
    c._track_job("printing")
    c._track_spools(3.0)
    check("next lookup wins", shown(c, "T0_knomi")[1], 0xFF8800)


def test_a_spool_without_a_colour_keeps_the_current_one():
    spoolman = Spoolman(s9=spool(None, ""))
    c, _ = row(spoolman, t0__spool_id=9)
    c.tool_state("T0_knomi").color = 0x123456
    c.tool_state("T0_knomi").type = "TPU"
    c._track_spools(1.0)
    check("kept", shown(c, "T0_knomi"), (9, 0x123456, "TPU"))


def test_two_screens_on_one_tool_share_one_lookup():
    spoolman = Spoolman(s9=spool("FF8800"))
    c = cluster(FakeSaveVariables(t0__spool_id=9), spoolman,
                FakeDevice("T0_knomi", "0"), FakeDevice("spare", "0"))
    c._track_spools(1.0)
    check("asked once", spoolman.asked, [9])
    check("spare", shown(c, "spare"), (9, 0xFF8800, "PLA"))


def test_screens_that_opt_out_or_have_no_tool_are_left_alone():
    spoolman = Spoolman(s9=spool("FF8800"))
    c = cluster(FakeSaveVariables(t0__spool_id=9), spoolman,
                FakeDevice("T0_knomi", "0", spoolman=False),
                FakeDevice("knomi_serial", None))
    c._track_spools(1.0)
    check("never asked", spoolman.asked, [])
    check("no tools touched", c.tools, {})


def test_no_save_variables_means_no_lookups():
    c, spoolman = row()
    c.save_variables = None
    c._track_spools(1.0)
    check("never asked", spoolman.asked, [])


def test_a_result_for_a_swapped_spool_is_dropped():
    c, spoolman = row(t0__spool_id=9)
    pending = []
    c._start_lookup = pending.append
    c._track_spools(1.0)
    c.save_variables.allVariables["t0__spool_id"] = 11
    c._track_spools(2.0)
    for work in pending:
        work()
    check("only the current spool shows", shown(c, "T0_knomi"),
          (11, 0x0000FF, "PETG"))


def test_one_lookup_at_a_time_per_spool():
    c, _ = row(t0__spool_id=9)
    pending = []
    c._start_lookup = pending.append
    c._track_spools(1.0)
    c._track_job("printing")
    c._track_spools(2.0)
    check("still one in flight", len(pending), 1)


def test_a_failed_lookup_keeps_the_colour_and_retries_for_a_while():
    c, spoolman = row(t0__spool_id=9)
    spoolman.down = True
    c.tool_state("T0_knomi").color = 0x123456
    # The fake reactor hands results back at t=100, so retries fall due from
    # 100 + the retry period.
    c._track_spools(1.0)
    check("kept", shown(c, "T0_knomi")[1], 0x123456)
    c._track_spools(100.0 + k._SPOOLMAN_RETRY_PERIOD - 1)
    check("not yet", len(spoolman.asked), 1)
    c._track_spools(100.0 + k._SPOOLMAN_RETRY_PERIOD)
    check("retried", len(spoolman.asked), 2)
    spoolman.down = False
    c._track_spools(100.0 + k._SPOOLMAN_RETRY_PERIOD)
    check("recovered", shown(c, "T0_knomi")[1], 0xFF8800)
    check("retry forgotten", c._spool_retry, {})


def test_retries_give_up_and_wait_for_the_next_trigger():
    c, spoolman = row(t0__spool_id=9)
    spoolman.down = True
    for _ in range(k._SPOOLMAN_RETRIES + 3):
        c._track_spools(1000.0)
    check("bounded", len(spoolman.asked), k._SPOOLMAN_RETRIES)
    c._track_job("printing")
    c._track_spools(1000.0)
    check("a print start tries again", len(spoolman.asked),
          k._SPOOLMAN_RETRIES + 1)


def test_an_unloaded_spool_is_not_retried():
    c, spoolman = row(t0__spool_id=9)
    spoolman.down = True
    c._track_spools(1.0)
    c.save_variables.allVariables["t0__spool_id"] = None
    c._track_spools(1000.0)
    check("dropped", c._spool_retry, {})
    check("asked once", spoolman.asked, [9])


def test_status_reports_the_spool():
    c, _ = row(t0__spool_id=9)
    c._track_spools(1.0)
    c.devices = []
    got = c.get_status(1.0)["screens"]["T0_knomi"]
    check("status", got, {"used": True, "filament_color": "FF8800",
                          "filament_type": "PLA", "spool_id": 9})


def main():
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and callable(fn)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print(f"  ok    {name}")
        except Exception as e:
            failed += 1
            print(f"  FAIL  {name}\n          {type(e).__name__}: {e}")
    print(f"\n  {len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
