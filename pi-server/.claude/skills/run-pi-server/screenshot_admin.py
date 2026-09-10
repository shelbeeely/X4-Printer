#!/usr/bin/env python3
"""Screenshots the admin console's Jobs tab with a real authenticated
Playwright session. Requires `pip install playwright` in the active venv
(no `playwright install` -- use the pre-installed Chromium below).

Usage: screenshot_admin.py <base_url> <admin_password> <out_path>
Example: screenshot_admin.py https://127.0.0.1:8090 smoketest123 /tmp/admin.png
"""
import sys

from playwright.sync_api import sync_playwright

CHROMIUM_PATH = "/opt/pw-browsers/chromium"


def main() -> int:
    base_url, password, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    with sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=CHROMIUM_PATH)
        ctx = browser.new_context(
            ignore_https_errors=True,
            http_credentials={"username": "admin", "password": password},
        )
        page = ctx.new_page()
        page.goto(f"{base_url}/")
        page.click("button.tab-btn[data-tab='jobs']")
        page.wait_for_selector("#jobs-table tbody tr", timeout=10000)
        page.wait_for_timeout(300)
        page.screenshot(path=out_path)
        browser.close()
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
