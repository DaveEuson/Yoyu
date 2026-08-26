"""Unit tests for the companion's pure logic.

Deliberately limited to functions with no network, no filesystem and no Claude
login: window parsing/labelling and the pairing HMAC. Those are the parts that
can break silently — a mislabelled window looks plausible on the board, and a
pairing MAC that drifts from the firmware's framing fails only on real
hardware, which CI can't exercise.

Run: python -m unittest discover -s companion
"""

import contextlib
import hashlib
import hmac
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import companion  # noqa: E402


class WindowLabelTests(unittest.TestCase):
    def test_known_windows_use_their_explicit_names(self):
        self.assertEqual(companion._window_label("five_hour"), "Session (5 hour)")
        self.assertEqual(companion._window_label("seven_day"), "Weekly (all models)")
        self.assertEqual(companion._window_label("seven_day_opus"), "Weekly (Opus)")
        self.assertEqual(companion._window_label("seven_day_fable"), "Weekly (Fable)")

    def test_unknown_model_window_is_named_after_the_model(self):
        # A model Anthropic ships later must not read as "Seven Day Haiku".
        self.assertEqual(companion._window_label("seven_day_haiku"), "Weekly (Haiku)")
        self.assertEqual(companion._window_label("seven_day_new_model"),
                         "Weekly (New Model)")

    def test_unrelated_key_falls_back_to_title_case(self):
        self.assertEqual(companion._window_label("extra_usage"), "Extra usage")
        self.assertEqual(companion._window_label("some_future_thing"),
                         "Some Future Thing")


class WindowsFromUsageTests(unittest.TestCase):
    def test_orders_known_windows_and_appends_unknown_ones(self):
        raw = {
            "seven_day_opus": {"utilization": 10},
            "five_hour": {"utilization": 20},
            "zzz_unknown": {"utilization": 30},
            "seven_day": {"utilization": 40},
        }
        keys = [w["key"] for w in companion.windows_from_usage(raw)]
        self.assertEqual(keys[:3], ["five_hour", "seven_day", "seven_day_opus"])
        self.assertEqual(keys[-1], "zzz_unknown")   # unknown sorts last, not dropped

    def test_utilization_is_clamped_and_rounded(self):
        raw = {
            "five_hour": {"utilization": 150},     # over 100
            "seven_day": {"utilization": -5},      # under 0
            "seven_day_opus": {"utilization": 33.333},
        }
        got = {w["key"]: w["utilization"] for w in companion.windows_from_usage(raw)}
        self.assertEqual(got["five_hour"], 100.0)
        self.assertEqual(got["seven_day"], 0.0)
        self.assertEqual(got["seven_day_opus"], 33.3)

    def test_skips_entries_that_are_not_usable(self):
        raw = {
            "five_hour": {"utilization": 5},
            "no_util": {"resets_at": "2026-01-01T00:00:00Z"},  # missing utilization
            "not_a_dict": 7,
            "bad_util": {"utilization": "banana"},
        }
        keys = [w["key"] for w in companion.windows_from_usage(raw)]
        self.assertEqual(keys, ["five_hour"])

    def test_accepts_either_resets_at_spelling(self):
        raw = {
            "five_hour": {"utilization": 1, "resets_at": "A"},
            "seven_day": {"utilization": 1, "resetsAt": "B"},
        }
        got = {w["key"]: w["resets_at"] for w in companion.windows_from_usage(raw)}
        self.assertEqual(got["five_hour"], "A")
        self.assertEqual(got["seven_day"], "B")

    def test_empty_or_none_input_is_not_an_error(self):
        self.assertEqual(companion.windows_from_usage(None), [])
        self.assertEqual(companion.windows_from_usage({}), [])


class PairHmacTests(unittest.TestCase):
    """The board computes HMAC-SHA256(code, message) with mbedtls and compares
    hex strings. If either side's framing drifts, pairing fails only on real
    hardware — so pin the exact bytes here."""

    def test_matches_an_independently_computed_digest(self):
        code, nonce = "SUE9HE", b"0123456789abcdef"
        expected = hmac.new(code.encode(), nonce, hashlib.sha256).hexdigest()
        self.assertEqual(companion._pair_hmac(code, nonce), expected)

    def test_is_lowercase_hex_of_the_right_length(self):
        mac = companion._pair_hmac("ABC123", b"x")
        self.assertEqual(len(mac), 64)              # sha256 -> 32 bytes -> 64 hex
        self.assertEqual(mac, mac.lower())
        int(mac, 16)                                # must parse as hex

    def test_token_mac_covers_nonce_concatenated_with_body(self):
        # Firmware: hmacSha256Hex(pairCode, nonce + body). Concatenation order
        # matters and is not otherwise exercised until a real pairing.
        code, nonce, body = "CODE12", b"NONCE", b'{"a":1}'
        self.assertEqual(companion._pair_hmac(code, nonce + body),
                         hmac.new(code.encode(), nonce + body,
                                  hashlib.sha256).hexdigest())

    def test_a_different_code_produces_a_different_mac(self):
        msg = b"same-message"
        self.assertNotEqual(companion._pair_hmac("AAAAAA", msg),
                            companion._pair_hmac("BBBBBB", msg))


class ActionKeyTests(unittest.TestCase):
    """Key combos are turned into OS calls. Getting a combo wrong types the
    wrong thing into whatever the user has focused, so parsing is pinned here
    and unknown combos must be rejected rather than half-sent."""

    def setUp(self):
        self.calls = []
        self._real_run = companion.subprocess.run
        companion.subprocess.run = lambda *a, **k: (
            self.calls.append(a[0]) or _FakeProc())

    def tearDown(self):
        companion.subprocess.run = self._real_run

    def test_macos_named_keys_and_modifiers(self):
        self.assertTrue(companion._send_keys_macos("shift+tab"))
        self.assertIn("key code 48 using {shift down}", self.calls[-1][-1])

    def test_macos_plain_character_uses_keystroke(self):
        self.assertTrue(companion._send_keys_macos("ctrl+c"))
        self.assertIn('keystroke "c" using {control down}', self.calls[-1][-1])

    def test_linux_builds_xdotool_combo(self):
        self.assertTrue(companion._send_keys_linux("shift+tab"))
        self.assertEqual(self.calls[-1], ["xdotool", "key", "shift+Tab"])

    def test_unknown_combos_are_rejected_not_partially_sent(self):
        for combo in ("bogus", "shift", "ctrl+nonsense"):
            self.calls.clear()
            self.assertFalse(companion._send_keys_macos(combo), combo)
            self.assertFalse(companion._send_keys_linux(combo), combo)
            self.assertEqual(self.calls, [], f"{combo!r} sent something")

    def test_default_actions_match_what_the_firmware_offers(self):
        # The board queues these ids; an unmapped one would silently do nothing.
        self.assertEqual(set(companion.DEFAULT_ACTION_KEYS),
                         {"voice", "mode", "cancel"})


class ProjectNameTests(unittest.TestCase):
    """Naming the project a token belongs to.

    The folder under ~/.claude/projects is path-mangled and genuinely
    ambiguous: 'H--Projects-Kiosk-Grand' could be 'Kiosk-Grand' or
    'Kiosk Grand' (it is the latter), and no amount of splitting on '-'
    recovers that. These pin the rule that `cwd` wins, because getting it
    wrong produces a plausible-looking board screen with the wrong labels.
    """

    ROOT = os.path.join("home", ".claude", "projects")

    def _name(self, entry, slug):
        key = companion._project_key(
            entry, os.path.join(self.ROOT, slug, "s.jsonl"), self.ROOT)
        return companion._project_name(key)

    def test_cwd_wins_over_the_mangled_slug(self):
        self.assertEqual(
            self._name({"cwd": r"H:\Projects\Kiosk Grand"},
                       "H--Projects-Kiosk-Grand"),
            "Kiosk Grand")

    def test_names_containing_separators_survive(self):
        for cwd, want in ((r"H:\Projects\RigMatch.AI-main", "RigMatch.AI-main"),
                          ("/home/dave/my-app", "my-app"),
                          ("/srv/Website 2", "Website 2")):
            self.assertEqual(self._name({"cwd": cwd}, "ignored"), want, cwd)

    def test_trailing_separators_dont_yield_an_empty_name(self):
        for cwd, want in ((r"H:\Projects\Thing" + "\\", "Thing"),
                          ("/home/dave/thing/", "thing")):
            self.assertEqual(self._name({"cwd": cwd}, "x--y-thing"), want, cwd)

    def test_falls_back_to_the_slug_when_cwd_is_missing_or_junk(self):
        for entry in ({}, {"cwd": ""}, {"cwd": "   "}, {"cwd": None}):
            self.assertEqual(self._name(entry, "H--Projects-Sparko"), "Sparko")


class ProjectRollupTests(unittest.TestCase):
    """Folding nested cwds into the project a person would name.

    Claude Code keys a project off the cwd, so one repo opened at three depths
    is three rows, each understating the work.
    """

    def test_nested_paths_fold_into_a_tracked_ancestor(self):
        got = companion._roll_up_nested({
            "H:/Projects/Rig": 10,
            "H:/Projects/Rig/Rig": 70,
            "H:/Projects/Rig/Rig/chat/src-tauri": 5,
        })
        self.assertEqual(got, {"H:/Projects/Rig": 85})

    def test_an_untracked_ancestor_is_not_invented(self):
        # No H:/Projects/Qibb project exists, so its children stay separate
        # rather than being grouped under a directory nobody worked in.
        totals = {"H:/Projects/Qibb/Audio to Video": 20,
                  "H:/Projects/Qibb/Video to Audio": 5}
        self.assertEqual(companion._roll_up_nested(totals), totals)

    def test_matching_is_case_insensitive(self):
        got = companion._roll_up_nested({
            "H:/Projects/sparko": 30,
            "h:/projects/SPARKO/sub": 1,
        })
        self.assertEqual(got, {"H:/Projects/sparko": 31})

    def test_tokens_are_never_lost_or_duplicated(self):
        totals = {"/a": 3, "/a/b": 5, "/a/b/c": 7, "/d": 11, "/e/f": 13}
        got = companion._roll_up_nested(totals)
        self.assertEqual(sum(got.values()), sum(totals.values()))
        self.assertEqual(got["/a"], 15)

    def test_siblings_are_left_alone(self):
        totals = {"/w/api": 1, "/w/web": 2}
        self.assertEqual(companion._roll_up_nested(totals), totals)


class ProjectLabelTests(unittest.TestCase):
    """Turning project paths into board rows.

    Two different projects must never render as the same row — a merged or
    duplicated label is a wrong number presented confidently, which is the one
    failure mode a usage display can't afford.
    """

    def test_same_basename_is_qualified_by_its_parent(self):
        got = companion._label_projects(
            ["/home/d/work/client-a/web", "/home/d/work/client-b/web"])
        self.assertEqual(set(got.values()), {"client-a/web", "client-b/web"})

    def test_unique_basenames_are_left_alone(self):
        got = companion._label_projects(["/a/sparko", "/b/ClaudeTrackerPi"])
        self.assertEqual(set(got.values()), {"sparko", "ClaudeTrackerPi"})

    def test_labels_fit_the_board_and_stay_distinct(self):
        keys = ["/x/" + "averylongprojectname%d" % i for i in range(3)]
        got = companion._label_projects(keys, width=21)
        self.assertEqual(len(set(got.values())), 3, got)
        for label in got.values():
            self.assertLessEqual(len(label), 21, label)

    def test_long_qualified_labels_keep_the_part_that_distinguishes(self):
        # Real case from a bench run: a project nested inside a directory of
        # the same name. Trimming to the last N chars kept the shared basename
        # and destroyed the parent, yielding 'main/RigMatch.AI-main' and
        # 'ects/RigMatch.AI-main' — distinct only by a mangled prefix.
        got = companion._label_projects(
            ["H:/Projects/RigMatch.AI-main/RigMatch.AI-main",
             "H:/Projects/RigMatch.AI-main"], width=21)
        labels = list(got.values())
        self.assertEqual(len(set(labels)), 2, labels)
        for label in labels:
            self.assertLessEqual(len(label), 21, label)
            head = label.split("/")[0]
            self.assertFalse(head.startswith("ects"), label)
            self.assertTrue(
                "RigMatch.AI-main".startswith(head) or "Projects".startswith(head),
                f"{head!r} is a fragment, not a prefix of a real directory")

    def test_every_key_gets_exactly_one_label(self):
        keys = ["/a/web", "/b/web", "/c/api"]
        got = companion._label_projects(keys)
        self.assertEqual(sorted(got), sorted(keys))
        self.assertEqual(len(set(got.values())), 3)


class _FakeProc:
    returncode = 0


class StaleAutostartSweepTests(unittest.TestCase):
    """The sweep runs unprompted against somebody else's Startup folder, so the
    thing worth testing is what it refuses to delete."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, True)
        self._nt = os.name
        # The sweep is a no-op off Windows; pretend so the logic is reachable.
        os.name = "nt"
        self.addCleanup(lambda: setattr(os, "name", self._nt))

    def _write(self, name, body):
        p = os.path.join(self.dir, name)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(body)
        return p

    BAT = ('@echo off\n'
           r'start "" "C:\python\pythonw.exe" '
           r'"H:\Projects\ClaudeTrackerPi\companion\companion.py"' '\n')

    def test_removes_every_older_name(self):
        stale = [self._write(n, self.BAT)
                 for n in companion._WIN_AUTOSTART_NAMES[1:]]
        removed = companion.sweep_stale_autostart(self.dir)
        self.assertCountEqual(removed, stale)
        for p in stale:
            self.assertFalse(os.path.exists(p))

    def test_keeps_the_current_name(self):
        # The live entry is what makes the companion start at all; sweeping it
        # would silently disable auto-start every single run.
        current = self._write(companion._WIN_AUTOSTART_NAMES[0], self.BAT)
        self.assertEqual(companion.sweep_stale_autostart(self.dir), [])
        self.assertTrue(os.path.exists(current))

    def test_leaves_a_same_named_file_that_is_not_ours(self):
        # Deleting an unrelated file out of Startup because it shares a name
        # would be far worse than the bug this fixes.
        other = self._write(companion._WIN_AUTOSTART_NAMES[1],
                            '@echo off\n' r'start "" "C:\Games\launcher.exe"' '\n')
        self.assertEqual(companion.sweep_stale_autostart(self.dir), [])
        self.assertTrue(os.path.exists(other))

    def test_no_op_when_nothing_is_there(self):
        self.assertEqual(companion.sweep_stale_autostart(self.dir), [])


class EntryPointSweepTests(unittest.TestCase):
    """Every way of starting the companion must run the stale-autostart sweep.

    Structural rather than behavioural on purpose: tray.py imports pystray,
    which is not a test dependency, so this reads the source instead of the
    module. It exists because the sweep shipped in v1.6.1 reaching only one of
    the two entry points -- the CLI swept, the tray app did not, and the tray
    app is the build most people download. Source-level is enough to catch an
    entry point that forgets.
    """

    def _src(self, name):
        path = os.path.join(os.path.dirname(os.path.abspath(companion.__file__)),
                            name)
        with open(path, "r", encoding="utf-8") as fh:
            return fh.read()

    def test_cli_entry_point_sweeps(self):
        self.assertIn("sweep_stale_autostart()", self._src("companion.py"))

    def test_tray_entry_point_sweeps(self):
        self.assertIn("sweep_stale_autostart()", self._src("tray.py"))


class LoginStateTests(unittest.TestCase):
    """The four situations that used to be one message.

    They need different things done about them, and telling somebody "you're
    not signed in" when Claude Code is working in the next window reads as the
    tool being broken rather than as a diagnosis.
    """

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.claude = os.path.join(self.dir, ".claude")

    def _creds(self, obj):
        os.makedirs(self.claude, exist_ok=True)
        with open(os.path.join(self.claude, ".credentials.json"), "w",
                  encoding="utf-8") as fh:
            fh.write(obj if isinstance(obj, str) else json.dumps(obj))

    def test_not_installed_when_there_is_no_claude_dir(self):
        self.assertEqual(companion.login_state(self.claude), "not_installed")

    def test_never_signed_in_when_dir_exists_but_no_file(self):
        os.makedirs(self.claude)
        self.assertEqual(companion.login_state(self.claude), "never_signed_in")

    def test_ok_when_a_token_is_present(self):
        self._creds({"claudeAiOauth": {"accessToken": "sk-xxx",
                                       "refreshToken": "rt-xxx"}})
        self.assertEqual(companion.login_state(self.claude), "ok")

    def test_signed_out_when_tokens_are_blank(self):
        # The real-world case: Claude Code wrote the record back without its
        # secrets. Distinguishable from never-signed-in only by the keys being
        # present and empty, which is why this is checked and not inferred.
        self._creds({"claudeAiOauth": {"accessToken": "", "refreshToken": "",
                                       "expiresAt": 0,
                                       "subscriptionType": "max"}})
        self.assertEqual(companion.login_state(self.claude), "signed_out")

    def test_unreadable_when_the_file_is_not_json(self):
        self._creds("{ this is not json")
        self.assertEqual(companion.login_state(self.claude), "unreadable")

    def test_every_state_gives_a_terminal_command_to_run(self):
        # The whole point is that nobody is left wondering what to type.
        for st in ("not_installed", "never_signed_in", "signed_out", "unreadable"):
            text = "\n".join(companion.login_help(st))
            self.assertTrue("claude" in text,
                            f"{st} help never mentions the claude command")
            if st != "not_installed":
                self.assertIn("/login", text, f"{st} help omits /login")

    def test_signed_out_names_the_shared_account_cause(self):
        # Without this the user fixes it, and it breaks again tomorrow.
        text = "\n".join(companion.login_help("signed_out")).lower()
        self.assertIn("same claude account", text)
        self.assertIn("rotate", text)


class _QuietTest(unittest.TestCase):
    """Silences the functions under test. Several of them report what they did
    to stdout, which is right in the companion and noise in a test run."""

    def setUp(self):
        buf = io.StringIO()
        ctx = contextlib.redirect_stdout(buf)
        ctx.__enter__()
        self.addCleanup(ctx.__exit__, None, None, None)
        self.out = buf


class TopupKeyLookupTests(_QuietTest):
    """Which board a key belongs to.

    This is the part of two-board support that fails silently: the wrong answer
    doesn't crash, it just quietly stops topping a board up, and the board says
    it is waiting for your computer while the computer thinks it is done.
    """

    def setUp(self):
        super().setUp()
        self.keys = {}
        self.ids = {}          # url -> device id
        self.ips = {}          # url -> address it resolves to
        self.saved = []
        companion._ID_CACHE.clear()
        self.addCleanup(companion._ID_CACHE.clear)
        self._orig_board_id = companion.board_id
        for name, repl in (
                ("_topup_keys", lambda: dict(self.keys)),
                ("board_id", lambda u, refresh=False: self.ids.get(u.rstrip("/"))),
                ("_url_ip", lambda u: self.ips.get(u.rstrip("/"))),
                ("_merge_config", lambda **f: (self.keys.clear(),
                                               self.keys.update(f["topup_keys"]),
                                               self.saved.append(f))[0])):
            orig = getattr(companion, name)
            setattr(companion, name, repl)
            self.addCleanup(setattr, companion, name, orig)

    def test_id_wins_over_address(self):
        self.keys = {"ab12cd": "right", "http://192.168.0.9:8080": "wrong"}
        self.ids["http://192.168.0.9:8080"] = "ab12cd"
        self.assertEqual(companion.topup_key_for("http://192.168.0.9:8080"),
                         "right")

    def test_exact_address_is_used_when_the_board_has_no_id(self):
        # Firmware older than 1.7.0 has no id to give.
        self.keys = {"http://192.168.0.9:8080": "k"}
        self.assertEqual(companion.topup_key_for("http://192.168.0.9:8080"), "k")

    def test_a_key_paired_against_a_hostname_still_matches_the_board(self):
        # The real case: paired via yoyu.local, later addressed by the IP
        # discovery found. Same board, two spellings.
        self.keys = {"http://yoyu.local:8080": "k"}
        self.ips = {"http://yoyu.local:8080": "192.168.0.77",
                    "http://192.168.0.77:8080": "192.168.0.77"}
        self.assertEqual(companion.topup_key_for("http://192.168.0.77:8080"), "k")

    def test_the_other_board_does_not_borrow_that_key(self):
        # The bug this whole change exists to prevent.
        self.keys = {"http://yoyu.local:8080": "k"}
        self.ips = {"http://yoyu.local:8080": "192.168.0.77",
                    "http://192.168.0.76:8080": "192.168.0.76"}
        self.assertIsNone(companion.topup_key_for("http://192.168.0.76:8080"))

    def test_an_unreachable_board_is_asked_again_next_time(self):
        """Found on hardware: a board probed mid-reboot answered nothing, and
        the absence was cached, so it stayed id-less for the whole run."""
        calls = []
        replies = [None, {"id": "ab12cd"}]     # rebooting, then up

        def fake_probe(u, timeout=0.8):
            calls.append(u)
            return replies[min(len(calls) - 1, len(replies) - 1)]

        orig_probe = companion._probe_info
        orig_bid = self._orig_board_id
        companion._probe_info = fake_probe
        companion.board_id = orig_bid          # the real one, not the stub
        self.addCleanup(setattr, companion, "_probe_info", orig_probe)
        companion._ID_CACHE.clear()

        self.assertIsNone(companion.board_id("http://b:8080"))
        self.assertEqual(companion.board_id("http://b:8080"), "ab12cd")
        self.assertEqual(len(calls), 2, "a missing id must not be cached")

    def test_migration_refiles_under_the_id(self):
        self.keys = {"http://yoyu.local:8080": "k"}
        self.ids["http://yoyu.local:8080"] = "ab12cd"
        moved = companion.migrate_topup_keys()
        self.assertEqual(moved, {"ab12cd": "k"})
        self.assertEqual(self.keys, {"ab12cd": "k"})

    def test_migration_keeps_a_key_whose_board_is_switched_off(self):
        # board_id() returns None for an unreachable board. Dropping the key
        # would force a re-pair for nothing more than being powered down.
        self.keys = {"http://yoyu.local:8080": "k"}
        self.assertEqual(companion.migrate_topup_keys(), {})
        self.assertEqual(self.keys, {"http://yoyu.local:8080": "k"})

    def test_already_migrated_keys_are_left_alone(self):
        self.keys = {"ab12cd": "k"}
        self.assertEqual(companion.migrate_topup_keys(), {})
        self.assertEqual(self.keys, {"ab12cd": "k"})
        self.assertEqual(self.saved, [])       # and nothing rewritten


class ResolveTargetsTests(_QuietTest):
    """A saved address that stopped answering used to be a permanent trap: it
    was truthy, so discovery never ran again and every push went nowhere."""

    def setUp(self):
        super().setUp()
        self.alive = set()
        self.found = []
        self.saved = []
        for name, repl in (
                ("_probe", lambda u: u.rstrip("/") in self.alive),
                ("discover_all", lambda *a, **k: list(self.found)),
                ("save_pi", lambda u: self.saved.append(u))):
            orig = getattr(companion, name)
            setattr(companion, name, repl)
            self.addCleanup(setattr, companion, name, orig)

    def test_a_reachable_saved_board_is_kept_without_scanning(self):
        self.alive = {"http://192.168.0.76:8080"}
        self.found = [{"url": "http://192.168.0.99:8080", "id": "z",
                       "board": "lcd2", "version": "1.7.0"}]
        out = companion.resolve_targets("http://192.168.0.76:8080")
        self.assertEqual(out, "http://192.168.0.76:8080")
        self.assertEqual(self.saved, [])       # no scan, no rewrite

    def test_a_stale_saved_address_triggers_a_fresh_look(self):
        self.found = [{"url": "http://192.168.0.76:8080", "id": "a",
                       "board": "lcd2", "version": "1.7.0"},
                      {"url": "http://192.168.0.77:8080", "id": "b",
                       "board": "amoled216", "version": "1.7.0"}]
        out = companion.resolve_targets("http://headroom.local:8080")
        self.assertEqual(out, "http://192.168.0.76:8080,"
                              "http://192.168.0.77:8080")
        self.assertEqual(self.saved, [out])

    def test_rescan_looks_again_even_when_the_saved_board_answers(self):
        self.alive = {"http://192.168.0.76:8080"}
        self.found = [{"url": "http://192.168.0.76:8080", "id": "a",
                       "board": "lcd2", "version": "1.7.0"},
                      {"url": "http://192.168.0.77:8080", "id": "b",
                       "board": "amoled216", "version": "1.7.0"}]
        out = companion.resolve_targets("http://192.168.0.76:8080", rescan=True)
        self.assertIn("192.168.0.77", out)

    def test_finding_nothing_keeps_what_was_saved(self):
        # Everything off, or the laptop is on a different network. Forgetting
        # the boards here would mean re-pairing them for a temporary outage.
        out = companion.resolve_targets("http://192.168.0.76:8080")
        self.assertEqual(out, "http://192.168.0.76:8080")
        self.assertEqual(self.saved, [])


class DisconnectTests(_QuietTest):
    """Disconnect has to forget the key at this end too.

    The board revokes its own on disconnect, so a copy left here is dead
    weight that surfaces later as a puzzling refusal rather than as anything
    actionable.
    """

    def setUp(self):
        super().setUp()
        self.keys = {"ab12cd": "k", "http://other:8080": "z"}
        self.posted = []
        companion._ID_CACHE.clear()
        self.addCleanup(companion._ID_CACHE.clear)

        class Resp:
            status = 200
            def __enter__(self_): return self_
            def __exit__(self_, *a): return False

        for name, repl in (
                ("_topup_keys", lambda: dict(self.keys)),
                ("board_id", lambda u, refresh=False: "ab12cd"),
                ("_merge_config", lambda **f: (self.keys.clear(),
                                               self.keys.update(f["topup_keys"]))[0])):
            orig = getattr(companion, name)
            setattr(companion, name, repl)
            self.addCleanup(setattr, companion, name, orig)

        def fake_open(req, timeout=0):
            self.posted.append((req.full_url, req.get_method()))
            return Resp()
        orig = companion.urllib.request.urlopen
        companion.urllib.request.urlopen = fake_open
        self.addCleanup(setattr, companion.urllib.request, "urlopen", orig)

    def test_posts_to_the_boards_disconnect_endpoint(self):
        self.assertTrue(companion.disconnect_board("http://b:8080"))
        self.assertEqual(self.posted, [("http://b:8080/disconnect", "POST")])

    def test_drops_this_computers_key_for_that_board(self):
        companion.disconnect_board("http://b:8080")
        self.assertNotIn("ab12cd", self.keys)

    def test_leaves_other_boards_keys_alone(self):
        companion.disconnect_board("http://b:8080")
        self.assertEqual(self.keys, {"http://other:8080": "z"})

    def test_an_unreachable_board_keeps_its_key(self):
        # Forgetting the key because the board happened to be off would force a
        # re-pair for a temporary outage.
        def boom(req, timeout=0):
            raise companion.urllib.error.URLError("down")
        companion.urllib.request.urlopen = boom
        self.assertFalse(companion.disconnect_board("http://b:8080"))
        self.assertIn("ab12cd", self.keys)


class CreditsFromUsageTests(unittest.TestCase):
    """What happens after the plan limits run out.

    The shape here is copied from a real /api/oauth/usage response, because the
    reason this went unshown for so long is that `extra_usage.utilization` is
    null and windows_from_usage drops anything with a null utilization -- so
    "Extra usage" sat in WINDOW_LABELS as a label nothing could render.
    """

    REAL = {
        "five_hour": {"utilization": 10.0, "resets_at": "2026-08-26T23:59:59+00:00"},
        "seven_day": {"utilization": 99.0, "resets_at": "2026-08-27T23:59:59+00:00"},
        "extra_usage": {"is_enabled": True, "monthly_limit": 2000,
                        "used_credits": 0.0, "utilization": None,
                        "currency": "USD", "decimal_places": 2},
        "spend": {"used": {"amount_minor": 0, "currency": "USD", "exponent": 2},
                  "limit": {"amount_minor": 2000, "currency": "USD", "exponent": 2},
                  "percent": 0, "severity": "normal", "enabled": True},
    }

    @staticmethod
    def _spend(used, limit=2000, severity="normal", enabled=True, percent=None):
        return {"spend": {"enabled": enabled, "severity": severity,
                          "percent": percent,
                          "used": {"amount_minor": used, "currency": "USD",
                                   "exponent": 2},
                          "limit": {"amount_minor": limit, "currency": "USD",
                                    "exponent": 2}}}

    def test_reads_the_real_payload_shape(self):
        c = companion.credits_from_usage(self.REAL)
        self.assertEqual(c["used_minor"], 0)
        self.assertEqual(c["limit_minor"], 2000)
        self.assertEqual(c["currency"], "USD")
        self.assertFalse(c["limit_reached"])

    def test_credits_never_appear_as_a_usage_window(self):
        # The whole point of keeping them apart: a window would reach
        # tightestWindow() and through it the mascot, whose job is headroom.
        keys = [w["key"] for w in companion.windows_from_usage(self.REAL)]
        self.assertNotIn("extra_usage", keys)
        self.assertNotIn("spend", keys)

    def test_percent_is_derived_when_the_server_omits_it(self):
        c = companion.credits_from_usage(self._spend(500, percent=None))
        self.assertEqual(c["percent"], 25.0)

    def test_servers_percent_wins_when_given(self):
        c = companion.credits_from_usage(self._spend(500, percent=99))
        self.assertEqual(c["percent"], 99.0)

    def test_critical_severity_is_the_spend_cap(self):
        self.assertTrue(
            companion.credits_from_usage(
                self._spend(2000, severity="critical"))["limit_reached"])

    def test_disabled_or_absent_credits_show_nothing(self):
        # None means "draw no row", which is not the same as "spent nothing".
        self.assertIsNone(companion.credits_from_usage(self._spend(0, enabled=False)))
        self.assertIsNone(companion.credits_from_usage({}))
        self.assertIsNone(companion.credits_from_usage(None))

    def test_a_malformed_spend_block_is_ignored_not_guessed(self):
        self.assertIsNone(companion.credits_from_usage({"spend": {"enabled": True}}))
        self.assertIsNone(companion.credits_from_usage(
            {"spend": {"enabled": True, "used": {"amount_minor": "lots"}}}))

    def test_internal_codename_windows_are_not_shown(self):
        # nimbus_quill and friends come back beside the real windows. A board
        # with three meter slots was spending one on "Nimbus Quill 0%".
        keys = [w["key"] for w in companion.windows_from_usage({
            "five_hour": {"utilization": 10.0, "resets_at": "2026-08-26T00:00:00Z"},
            "nimbus_quill": {"utilization": 0.0, "resets_at": None},
            "tangelo": {"utilization": 0.0, "resets_at": None},
        })]
        self.assertEqual(keys, ["five_hour"])

    def test_an_unknown_window_with_a_reset_time_still_shows(self):
        # The seven_day_<model> case this code already goes out of its way to
        # handle: a model Anthropic ships later must not be filtered away.
        keys = [w["key"] for w in companion.windows_from_usage({
            "seven_day_newmodel": {"utilization": 0.0,
                                   "resets_at": "2026-08-27T00:00:00Z"},
        })]
        self.assertEqual(keys, ["seven_day_newmodel"])

    def test_an_unknown_window_being_used_still_shows(self):
        # No reset time but real usage on it: that is a limit doing something,
        # and hiding it would hide the thing the board exists to report.
        keys = [w["key"] for w in companion.windows_from_usage({
            "mystery": {"utilization": 42.0, "resets_at": None},
        })]
        self.assertEqual(keys, ["mystery"])

    def test_a_missing_cap_still_reports_what_was_spent(self):
        raw = {"spend": {"enabled": True, "percent": None, "severity": "normal",
                         "used": {"amount_minor": 512, "currency": "GBP",
                                  "exponent": 2},
                         "limit": {}}}
        c = companion.credits_from_usage(raw)
        self.assertEqual(c["used_minor"], 512)
        self.assertIsNone(c["limit_minor"])
        self.assertEqual(c["currency"], "GBP")


if __name__ == "__main__":
    unittest.main()
