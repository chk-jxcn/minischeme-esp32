"""The board's HTTP access rules against a real board (skipped without one):

    MINISCHEME_BOARD=192.168.2.119 MINISCHEME_PASSWORD=... python -m unittest tests/test_board_http.py -v

MINISCHEME_PASSWORD is the board's hardware password ([http] http_password);
without it the tests that need a protected board are skipped. The running
app should have a web page (album, memo, medicine ...) for the /app tests.
Nothing on the board is changed: only reads and the app's "list"-style call
(MINISCHEME_APP_CALL, default "list")."""
import gzip
import json
import os
import unittest
import urllib.error
import urllib.parse
import urllib.request

HOST = os.environ.get("MINISCHEME_BOARD", "")
PASSWORD = os.environ.get("MINISCHEME_PASSWORD", "")
APP_CALL = os.environ.get("MINISCHEME_APP_CALL", "list")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


def request(path, method="GET", headers=None, data=None):
    """(status, headers, body bytes); HTTP errors are results too"""
    req = urllib.request.Request(f"http://{HOST}{path}", data=data, method=method, headers=headers or {})
    try:
        with OPENER.open(req, timeout=15) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()


def page_token():
    status, headers, _ = request("/app")
    if status != 302:
        return None
    loc = headers.get("Location", "")
    return urllib.parse.parse_qs(urllib.parse.urlsplit(loc).query).get("t", [None])[0]


@unittest.skipUnless(HOST, "set MINISCHEME_BOARD to the board's address")
class TestBoardHttp(unittest.TestCase):
    def test_info_answers_this_network(self):
        status, _, body = request("/api/info", headers={"Origin": "http://192.168.1.20"})
        self.assertIn(status, (200, 401))
        if status == 200:
            self.assertIn("api", json.loads(body))

    def test_other_sites_are_refused(self):
        status, _, body = request("/api/info", headers={"Origin": "https://example.com"})
        self.assertEqual(status, 403, body)

    def test_a_sandboxed_page_may_call_only_the_app_routes(self):
        status, _, _ = request("/api/info", headers={"Origin": "null"})
        self.assertEqual(status, 403)

    @unittest.skipUnless(PASSWORD, "set MINISCHEME_PASSWORD (a board with a hardware password)")
    def test_the_password_guards_the_api(self):
        self.assertEqual(request("/api/settings")[0], 401)
        self.assertEqual(request("/api/settings", headers={"X-MiniScheme-Auth": PASSWORD + "x"})[0], 401)
        self.assertEqual(request("/api/settings", headers={"X-MiniScheme-Auth": PASSWORD})[0], 200)


@unittest.skipUnless(HOST, "set MINISCHEME_BOARD to the board's address")
class TestAppPage(unittest.TestCase):
    """/app?t=<token>: the app's own page, the only one that may call its app"""

    @classmethod
    def setUpClass(cls):
        if HOST and request("/app")[0] == 404:
            raise unittest.SkipTest("the running app has no web page")
        cls.token = page_token() if HOST else None

    def call(self, token, origin="null", body=b""):
        q = f"/api/app/call?fn={urllib.parse.quote(APP_CALL)}" + (f"&t={token}" if token is not None else "")
        return request(q, "POST", {"Origin": origin, "Content-Type": "application/x-www-form-urlencoded"}, body)

    def setUp(self):
        # firmware before 1.0.67 serves /app without a token: with a hardware
        # password its page's calls were all refused (401)
        self.assertIsNotNone(self.token, "/app does not redirect to /app?t=<token>")

    def test_app_redirects_to_its_token(self):
        status, headers, _ = request("/app")
        self.assertEqual(status, 302)
        self.assertRegex(headers.get("Location", ""), r"^/app\?t=[0-9a-f]{16}$")

    def test_page_is_sandboxed(self):
        status, headers, body = request(f"/app?t={self.token}", headers={"Accept-Encoding": "gzip"})
        self.assertEqual(status, 200)
        self.assertIn("sandbox", headers.get("Content-Security-Policy", ""))
        self.assertNotIn("allow-same-origin", headers.get("Content-Security-Policy", ""))
        html = gzip.decompress(body).decode("utf-8") if headers.get("Content-Encoding") == "gzip" else body.decode()
        self.assertIn("APP_PAGE_TOKEN", html, "the page's appweb.js must send the token back")

    def test_the_page_token_opens_the_app_routes(self):
        status, _, body = self.call(self.token)
        self.assertEqual(status, 200, body)
        self.assertNotIn(b'"success":false', body[:40])

    @unittest.skipUnless(PASSWORD, "set MINISCHEME_PASSWORD (a board with a hardware password)")
    def test_without_the_token_a_protected_board_refuses(self):
        # a sandboxed frame of another site has no token: browsers send no
        # Referer from an opaque origin, so this is all such a frame can send
        self.assertEqual(self.call(None)[0], 401)
        self.assertEqual(self.call("0" * 16)[0], 401)
        status, _, _ = request("/api/app/file?name=meds.txt", headers={"Origin": "null"})
        self.assertEqual(status, 401)

    def test_the_token_opens_nothing_else(self):
        status, _, _ = request(f"/api/settings?t={self.token}", headers={"Origin": "null"})
        self.assertEqual(status, 403)


if __name__ == "__main__":
    unittest.main()
