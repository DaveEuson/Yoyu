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
        # extra_usage used to be the example here. It is no longer a window at
        # all -- it is money, excluded by name -- and the case below already
        # covers the fallback this test is about.
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

    def test_cli_entry_point_rescans_on_a_timer(self):
        self.assertIn("refresh_targets(", self._src("companion.py"))

    def test_tray_entry_point_rescans_on_a_timer(self):
        # Same reasoning as the sweep above, and the same trap: the tray runs
        # its own loop rather than the CLI's, so a periodic job added to one
        # reaches the other only if somebody remembers. The tray is the build
        # most people download, so it is the one that must not be forgotten.
        src = self._src("tray.py")
        self.assertIn("auto_rescan(icon)", src)
        self.assertIn("RESCAN_EVERY_DEFAULT", src)

    def test_both_entry_points_report_wrong_firmware(self):
        # Third time this pattern has mattered. A board on the wrong image has
        # no working screen, so the companion is the only thing that can say
        # so, and saying it in only one of the two builds helps whichever half
        # of people did not download that one.
        self.assertIn("warn_about_hardware(", self._src("companion.py"))
        self.assertIn("announce_wrong_firmware(", self._src("tray.py"))

    def test_tray_rescan_does_not_save_an_empty_sweep(self):
        # discover_boards saves whatever it finds, and a sweep finds nothing
        # when the network is unhappy as well as when the boards are gone.
        # The periodic path must go through refresh_targets, which guards it.
        src = self._src("tray.py")
        self.assertIn("companion.refresh_targets(saved)", src)


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

    def test_spend_still_shows_after_credits_are_switched_off(self):
        """Found on a live account that had gone over: $41.24 of a $40 cap with
        enabled=false, and the first version of this returned None.

        "enabled: false" means credits can no longer be SPENT -- the cap was
        hit, or an org disabled them. It does not mean the account never had
        any, and money already gone is still money gone. Hiding it at exactly
        the moment the figure matters most defeats the feature.
        """
        c = companion.credits_from_usage(
            self._spend(4124, limit=4000, severity="critical", enabled=False,
                        percent=100))
        self.assertIsNotNone(c)
        self.assertEqual(c["used_minor"], 4124)
        self.assertTrue(c["limit_reached"])
        self.assertFalse(c["available"])

    def test_extra_usage_never_becomes_a_window_even_when_it_has_a_number(self):
        """The bug the account going over actually exposed.

        extra_usage carries utilization: null only while UNUSED. Once credits
        are spent it reports a real number, sails through the null check into
        the window list, becomes the tightest window and drives the mascot --
        so the board showed "out of tokens" with the plan windows at 10% and
        1%. Excluding it cannot depend on a field's value.
        """
        keys = [w["key"] for w in companion.windows_from_usage({
            "five_hour": {"utilization": 10.0, "resets_at": "2026-08-26T00:00:00Z"},
            "extra_usage": {"utilization": 100.0, "is_enabled": False,
                            "used_credits": 4124.0},
            "spend": {"percent": 100, "enabled": False},
        })]
        self.assertEqual(keys, ["five_hour"])

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


class Reject401Tests(_QuietTest):
    """A 401 whose expiresAt still looks fine.

    Refresh tokens rotate, so anything else using the same login can kill this
    access token while our own clock still says it is good. Before this, the
    clock-based check handed back the same dead token every cycle and the
    boards stayed dark until somebody noticed and ran `claude /login`. Caught
    in the wild, not in a test.
    """

    def setUp(self):
        super().setUp()
        self.forced = []          # every valid_token(force=) we were asked for
        self.fetches = 0
        self.reads = 0
        creds = {"accessToken": "dead", "refreshToken": "r",
                 "expiresAt": 9e18, "_raw": {}, "subscriptionType": "max"}

        def fake_read():
            self.reads += 1
            return dict(creds), (lambda o: None)

        def fake_valid(c, save, force=False):
            self.forced.append(force)
            return "fresh" if force else "dead"

        for name, repl in (("read_creds", fake_read),
                           ("valid_token", fake_valid)):
            orig = getattr(companion, name)
            setattr(companion, name, repl)
            self.addCleanup(setattr, companion, name, orig)

    def _fetch(self, results):
        """results: list of "ok" or an HTTP code to raise."""
        def f(token):
            self.fetches += 1
            r = results[min(self.fetches - 1, len(results) - 1)]
            if r == "ok":
                return {"five_hour": {"utilization": 5.0,
                                      "resets_at": "2026-08-27T00:00:00Z"}}
            raise companion.urllib.error.HTTPError(
                companion.USAGE_URL, r, "no", {}, None)
        orig = companion.fetch_usage
        companion.fetch_usage = f
        self.addCleanup(setattr, companion, "fetch_usage", orig)

    def test_a_401_forces_one_refresh_and_retries(self):
        self._fetch([401, "ok"])
        windows, plan, credits = companion.get_live_windows()
        self.assertEqual(self.fetches, 2)
        self.assertEqual(self.forced, [False, True])   # cached, then forced
        self.assertEqual([w["key"] for w in windows], ["five_hour"])

    def test_creds_are_re_read_before_the_forced_refresh(self):
        # The first call may already have written a new refresh token back;
        # refreshing with the stale one in memory spends a dead token.
        self._fetch([401, "ok"])
        companion.get_live_windows()
        self.assertEqual(self.reads, 2)

    def test_a_second_401_gives_up_and_says_what_to_do(self):
        self._fetch([401, 401])
        with self.assertRaises(companion.LiveUnavailable) as cm:
            companion.get_live_windows()
        self.assertEqual(self.fetches, 2)              # exactly two, no loop
        self.assertIn("/login", str(cm.exception))

    def test_other_errors_are_not_retried(self):
        # A 429 must back off, not burn a refresh token retrying.
        self._fetch([429, "ok"])
        with self.assertRaises(companion.LiveUnavailable) as cm:
            companion.get_live_windows()
        self.assertEqual(self.fetches, 1)
        self.assertTrue(cm.exception.rate_limited)

    def test_a_healthy_call_never_forces_a_refresh(self):
        self._fetch(["ok"])
        companion.get_live_windows()
        self.assertEqual(self.fetches, 1)
        self.assertEqual(self.forced, [False])


class InstallTests(_QuietTest):
    """Where the login item points.

    The bug this replaced: autostart recorded sys.executable, so a binary run
    straight out of Downloads registered that path, and emptying Downloads
    broke start-up with nothing on screen to say so.
    """

    def setUp(self):
        super().setUp()
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.exe = os.path.join(self.dir, "YoyuCompanion.exe")
        orig = companion.installed_exe
        companion.installed_exe = lambda: self.exe
        self.addCleanup(setattr, companion, "installed_exe", orig)

    def test_launch_argv_prefers_the_installed_copy(self):
        with open(self.exe, "wb") as fh:
            fh.write(b"x")
        self.assertTrue(companion.is_installed())
        self.assertEqual(companion._launch_argv(), [self.exe])

    def test_launch_argv_falls_back_when_not_installed(self):
        # Nothing installed: the old behaviour is still the right one, because
        # there is nowhere better to point.
        self.assertFalse(companion.is_installed())
        self.assertNotEqual(companion._launch_argv(), [self.exe])

    def test_install_from_source_refuses_instead_of_pretending(self):
        # Not frozen, so there is no single file to copy. Silently doing
        # nothing would leave someone thinking they had installed it.
        with self.assertRaises(RuntimeError) as cm:
            companion.install_app()
        self.assertIn("--startup", str(cm.exception))

    def test_stale_upgrade_copy_is_swept(self):
        stale = self.exe + ".old"
        with open(stale, "wb") as fh:
            fh.write(b"x" * 10)
        self.assertEqual(companion.sweep_stale_install(), stale)
        self.assertFalse(os.path.exists(stale))

    def test_sweep_is_quiet_when_there_is_nothing_to_sweep(self):
        self.assertIsNone(companion.sweep_stale_install())


class PackagedInstallTests(_QuietTest):
    """A copy from apt must not be managed by this program.

    Without this, apt-installing the companion left the tray still offering
    "Install on this computer", which would copy /usr/bin's binary into
    ~/.local and leave two of them, with the package manager updating only
    the one you had stopped using.
    """

    def setUp(self):
        super().setUp()
        self._frozen = getattr(sys, "frozen", False)
        self._exe = sys.executable
        self._plat = companion.sys.platform
        self.addCleanup(self._restore)
        self.addCleanup(os.environ.pop, "APPIMAGE", None)

    def _restore(self):
        if self._frozen:
            sys.frozen = self._frozen
        elif hasattr(sys, "frozen"):
            del sys.frozen
        sys.executable = self._exe

    def _pretend(self, path):
        sys.frozen = True
        sys.executable = path

    def test_usr_bin_is_a_package(self):
        self._pretend("/usr/bin/yoyu-companion")
        if companion.sys.platform == "win32":
            self.skipTest("prefix test is for the platforms that have them")
        self.assertTrue(companion.packaged_install())
        self.assertTrue(companion.is_installed())
        self.assertTrue(companion.running_from_install())

    def test_a_downloaded_binary_is_not_a_package(self):
        self._pretend(os.path.join(tempfile.gettempdir(), "YoyuCompanion"))
        self.assertFalse(companion.packaged_install())

    def test_a_path_merely_starting_with_usr_is_not_a_package(self):
        # /usrlocal/... shares a prefix with /usr but is not inside it. A
        # startswith() without the separator would call this a package.
        self._pretend("/usrlocal/yoyu-companion")
        self.assertFalse(companion.packaged_install())

    def test_running_from_source_is_never_a_package(self):
        if hasattr(sys, "frozen"):
            del sys.frozen
        sys.executable = "/usr/bin/python3"
        self.assertFalse(companion.packaged_install())

    def test_install_refuses_to_copy_a_packaged_build(self):
        self._pretend("/usr/bin/yoyu-companion")
        if companion.sys.platform == "win32":
            self.skipTest("prefix test is for the platforms that have them")
        with self.assertRaises(RuntimeError) as cm:
            companion.install_app()
        self.assertIn("package manager", str(cm.exception))

    def test_appimage_installs_the_appimage_not_the_mount(self):
        # Inside an AppImage, sys.executable points into a temporary mount
        # that stops existing when the app quits. $APPIMAGE is the real file.
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        real = os.path.join(d, "YoyuCompanion.AppImage")
        with open(real, "wb") as fh:
            fh.write(b"x")
        os.environ["APPIMAGE"] = real
        self._pretend("/tmp/.mount_abc123/usr/bin/yoyu-companion")
        self.assertEqual(companion.appimage_path(), os.path.abspath(real))
        self.assertEqual(companion.self_path(), os.path.abspath(real))

    def test_appimage_var_pointing_at_nothing_is_ignored(self):
        os.environ["APPIMAGE"] = "/nowhere/does/this/exist.AppImage"
        self._pretend("/tmp/.mount_abc/usr/bin/yoyu-companion")
        self.assertEqual(companion.appimage_path(), "")
        self.assertEqual(companion.self_path(),
                         os.path.abspath(sys.executable))


class RefreshTargetsTests(_QuietTest):
    """Picking up a board you plugged in after the companion started.

    resolve_targets() stops as soon as one saved board answers, which is
    right for a stale address and wrong for a new one. That left adding a
    second board doing nothing until you knew --rescan existed.
    """

    A = "http://192.168.0.76:8080"
    B = "http://192.168.0.77:8080"
    C = "http://10.0.0.5:8080"

    def _discovery(self, urls):
        self._found = [{"url": u, "id": u[-6:], "board": "lcd2",
                        "version": "1.11.0"} for u in urls]
        orig = companion.discover_all
        companion.discover_all = lambda *a, **k: list(self._found)
        self.addCleanup(setattr, companion, "discover_all", orig)

    def _reachable(self, urls):
        orig = companion._probe
        companion._probe = lambda u: u in urls
        self.addCleanup(setattr, companion, "_probe", orig)

    def test_a_new_board_is_picked_up(self):
        self._discovery([self.A, self.B])
        self._reachable([self.A, self.B])
        targets, added, dropped = companion.refresh_targets(self.A)
        self.assertEqual(targets, self.A + "," + self.B)
        self.assertEqual([b["url"] for b in added], [self.B])
        self.assertEqual(dropped, [])

    def test_nothing_new_changes_nothing(self):
        # No save, no log line. A quiet network should stay quiet.
        self._discovery([self.A, self.B])
        self._reachable([self.A, self.B])
        targets, added, dropped = companion.refresh_targets(
            self.A + "," + self.B)
        self.assertEqual(targets, self.A + "," + self.B)
        self.assertEqual(added, [])
        self.assertEqual(dropped, [])

    def test_a_failed_sweep_never_empties_the_config(self):
        # Finding nothing means the network is unhappy, not that the boards
        # have been thrown away. Dropping them here would stop the companion
        # feeding anything until someone noticed.
        self._discovery([])
        self._reachable([])
        targets, added, dropped = companion.refresh_targets(
            self.A + "," + self.B)
        self.assertEqual(targets, self.A + "," + self.B)
        self.assertEqual(added, [])
        self.assertEqual(dropped, [])

    def test_a_board_off_the_local_subnet_is_kept(self):
        # discover_all only sweeps this machine's own ranges, so a board
        # reached across a router is invisible to it. It still answers, so
        # it stays.
        self._discovery([self.A])
        self._reachable([self.A, self.C])
        targets, added, dropped = companion.refresh_targets(
            self.A + "," + self.C)
        self.assertEqual(targets, self.A + "," + self.C)
        self.assertEqual(dropped, [])

    def test_a_board_that_stopped_answering_is_dropped(self):
        # Safe to drop precisely because this now runs on a timer: when it
        # comes back, the next sweep finds it again.
        self._discovery([self.A])
        self._reachable([self.A])
        targets, added, dropped = companion.refresh_targets(
            self.A + "," + self.B)
        self.assertEqual(targets, self.A)
        self.assertEqual(dropped, [self.B])

    def test_a_board_that_moved_address_is_followed(self):
        # DHCP hands it a new address. The old one stops answering and the
        # new one is discovered, so the list follows it rather than keeping
        # a dead entry forever.
        self._discovery([self.B])
        self._reachable([self.B])
        targets, added, dropped = companion.refresh_targets(self.A)
        self.assertEqual(targets, self.B)
        self.assertEqual([b["url"] for b in added], [self.B])
        self.assertEqual(dropped, [self.A])

    def test_starting_from_nothing_saved(self):
        self._discovery([self.A])
        self._reachable([self.A])
        targets, added, dropped = companion.refresh_targets("")
        self.assertEqual(targets, self.A)
        self.assertEqual([b["url"] for b in added], [self.A])


class RescanIntervalTests(_QuietTest):
    """0 has to mean "stop looking", not "I didn't say"."""

    def test_unset_gets_the_default(self):
        self.assertEqual(companion.rescan_interval(None),
                         companion.RESCAN_EVERY_DEFAULT)

    def test_zero_means_never(self):
        # The bug this guards: `cfg.get(k) or DEFAULT` treats 0 as absent, so
        # turning the sweep off would have turned it on at the default.
        self.assertEqual(companion.rescan_interval(0), 0)

    def test_a_number_is_honoured(self):
        self.assertEqual(companion.rescan_interval(60), 60)

    def test_the_flag_beats_the_config(self):
        self.assertEqual(companion.rescan_interval(900, arg=0), 0)
        self.assertEqual(companion.rescan_interval(900, arg=60), 60)

    def test_pinned_targets_are_never_second_guessed(self):
        # --pi says which board is meant. Finding others and feeding them is
        # not what was asked for.
        self.assertEqual(companion.rescan_interval(900, pinned=True), 0)
        self.assertEqual(companion.rescan_interval(900, arg=60, pinned=True), 0)

    def test_rubbish_falls_back_rather_than_crashing(self):
        self.assertEqual(companion.rescan_interval("nonsense"),
                         companion.RESCAN_EVERY_DEFAULT)

    def test_negative_is_clamped_not_trusted(self):
        self.assertEqual(companion.rescan_interval(-5), 0)


class WrongFirmwareTests(_QuietTest):
    """A board running the other board's image.

    It boots, joins Wi-Fi and answers every request while driving a panel that
    is not there, so from the network it looks perfectly healthy and from the
    desk it looks like hardware that will not turn on. This happened to a real
    board, and the screen is the one part that cannot report it.
    """

    GOOD = {"url": "http://10.0.0.1:8080", "id": "aaa", "board": "lcd2",
            "version": "1.13.0", "hw_ok": True, "hw_note": ""}
    BAD = {"url": "http://10.0.0.2:8080", "id": "bbb", "board": "amoled216",
           "version": "1.13.0", "hw_ok": False,
           "hw_note": "Wrong firmware: this is amoled216 firmware on lcd2 "
                      "hardware."}

    def test_a_healthy_board_says_nothing(self):
        self.assertEqual(companion.hardware_warning(self.GOOD), "")
        self.assertNotIn("WRONG", companion.describe_board(self.GOOD))

    def test_a_wrong_board_is_flagged_in_its_description(self):
        self.assertIn("WRONG FIRMWARE", companion.describe_board(self.BAD))

    def test_the_warning_says_ota_will_not_fix_it(self):
        # The trap: /update on such a board fetches the wrong image again,
        # because the running firmware decides which asset to ask for. Anyone
        # told only "it is running the wrong image" would reasonably try OTA
        # first and conclude the board is dead when nothing changes.
        w = companion.hardware_warning(self.BAD)
        self.assertIn("USB", w)
        self.assertIn("over-the-air", w)

    def test_firmware_without_the_field_is_treated_as_fine(self):
        # Every board in the field predates this. Absent means "cannot tell",
        # which must not read as "broken", or the companion would accuse
        # perfectly healthy boards on first run.
        old = {"url": "http://10.0.0.3:8080", "id": "ccc", "board": "lcd2",
               "version": "1.9.1"}
        self.assertEqual(companion.hardware_warning(old), "")
        self.assertNotIn("WRONG", companion.describe_board(old))


class AutostartTests(_QuietTest):
    """Start-at-login, for all three platforms, on whichever one runs this.

    This is the most platform-specific code in the companion and it had no
    tests at all, which is how three bugs lived in it: an unescaped macOS
    plist that launchd silently refused, a Linux install that crashed where
    there is no systemctl, and a Quit that only stuck on Windows. Every
    branch reads only sys.platform and a couple of environment variables, so
    each is driven here on every runner -- a Windows machine tests the macOS
    plist, and the CI matrix runs the lot natively on all three as well.
    """

    def setUp(self):
        super().setUp()
        self.home = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.home, True)
        # expanduser() reads HOME on POSIX and USERPROFILE on Windows; APPDATA
        # is where the Windows Startup folder lives. Point all three at the
        # sandbox so nothing here touches the real machine.
        for var in ("HOME", "USERPROFILE", "APPDATA"):
            self.addCleanup(self._restore_env, var, os.environ.get(var))
            os.environ[var] = self.home
        self._plat = companion.sys.platform
        self.addCleanup(setattr, companion.sys, "platform", self._plat)
        self.calls = []
        orig_run = companion._run_quietly
        companion._run_quietly = lambda cmd: self.calls.append(cmd)
        self.addCleanup(setattr, companion, "_run_quietly", orig_run)
        orig_argv = companion._launch_argv
        self.addCleanup(setattr, companion, "_launch_argv", orig_argv)

    @staticmethod
    def _restore_env(var, value):
        if value is None:
            os.environ.pop(var, None)
        else:
            os.environ[var] = value

    def _as(self, platform, argv):
        companion.sys.platform = platform
        companion._launch_argv = lambda: list(argv)

    # ---- macOS ---------------------------------------------------------
    def test_macos_plist_is_valid_xml_whatever_the_path(self):
        import plistlib
        path = os.path.join(self.home, "Sam & Alex <beta>", "YoyuCompanion")
        self._as("darwin", [path])
        target = companion.install_autostart()
        with open(target, "rb") as fh:
            plist = plistlib.load(fh)          # raises if launchd would refuse it
        self.assertEqual(plist["ProgramArguments"], [path])
        self.assertEqual(plist["Label"], "com.claudetracker.companion")

    def test_macos_restarts_after_a_crash_but_not_after_quit(self):
        import plistlib
        self._as("darwin", ["/Applications/YoyuCompanion"])
        with open(companion.install_autostart(), "rb") as fh:
            plist = plistlib.load(fh)
        self.assertTrue(plist["RunAtLoad"])
        # A plain True relaunched it the moment you chose Quit.
        self.assertEqual(plist["KeepAlive"], {"SuccessfulExit": False})

    def test_macos_loads_the_agent(self):
        self._as("darwin", ["/Applications/YoyuCompanion"])
        target = companion.install_autostart()
        self.assertIn(["launchctl", "load", target], self.calls)

    # ---- Linux ---------------------------------------------------------
    def test_linux_unit_restarts_only_on_failure(self):
        self._as("linux", ["/home/sam/.local/bin/yoyu-companion"])
        with open(companion.install_autostart(), encoding="utf-8") as fh:
            unit = fh.read()
        self.assertIn("Restart=on-failure", unit)
        self.assertNotIn("Restart=always", unit)
        self.assertIn("ExecStart=/home/sam/.local/bin/yoyu-companion", unit)

    def test_linux_enables_the_unit(self):
        self._as("linux", ["/usr/bin/yoyu-companion"])
        companion.install_autostart()
        self.assertTrue(any(c[:2] == ["systemctl", "--user"] for c in self.calls))

    # ---- Windows -------------------------------------------------------
    def test_windows_startup_entry_quotes_the_path(self):
        exe = os.path.join(self.home, "Program Files", "YoyuCompanion.exe")
        self._as("win32", [exe])
        target = companion.install_autostart()
        self.assertTrue(target.endswith("YoyuCompanion.bat"))
        with open(target, encoding="utf-8") as fh:
            bat = fh.read()
        # Quoted, or a space in the path splits it in two at login.
        self.assertIn('start "" "%s"' % exe, bat)

    def test_windows_needs_no_service_manager(self):
        self._as("win32", [os.path.join(self.home, "YoyuCompanion.exe")])
        companion.install_autostart()
        self.assertEqual(self.calls, [])


class ServiceManagerTests(_QuietTest):
    """A missing service manager must not take --install down with it."""

    def test_a_missing_command_is_tolerated(self):
        # Alpine, Void, WSL and most containers have no systemctl, and an
        # unguarded subprocess.run raises FileNotFoundError there.
        self.assertIsNone(
            companion._run_quietly(["yoyu-no-such-command-anywhere", "--help"]))


if __name__ == "__main__":
    unittest.main()
