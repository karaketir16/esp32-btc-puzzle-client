#!/usr/bin/env python3
"""Print Telegram chat IDs from pending bot updates (standard library only)."""

import argparse
import getpass
import json
import os
import re
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


def token_from_secrets_file():
    try:
        with open("secrets.h", encoding="utf-8") as secrets_file:
            contents = secrets_file.read()
    except OSError:
        return None
    match = re.search(
        r'^\s*#define\s+TELEGRAM_BOT_TOKEN\s+"([^"\n]+)"',
        contents,
        re.MULTILINE,
    )
    if match:
        token = match.group(1).strip()
        if token and not token.startswith("PASTE_"):
            return token
    return None


def fetch_updates(token, timeout_seconds):
    query = urlencode({"timeout": min(timeout_seconds, 10), "limit": 100})
    request = Request(
        f"https://api.telegram.org/bot{token}/getUpdates?{query}",
        headers={"User-Agent": "esp32-btc-puzzle-client-chat-id/1.0"},
    )
    try:
        with urlopen(request, timeout=min(timeout_seconds, 10) + 5) as response:
            payload = json.load(response)
    except HTTPError as exc:
        try:
            payload = json.loads(exc.read().decode("utf-8"))
            description = payload.get("description", "Telegram API request failed")
        except (ValueError, UnicodeDecodeError):
            description = f"Telegram returned HTTP {exc.code}"
        raise RuntimeError(description) from None
    except (URLError, TimeoutError, OSError) as exc:
        raise RuntimeError(f"Could not contact Telegram: {exc.reason if isinstance(exc, URLError) else exc}") from None
    if not payload.get("ok"):
        raise RuntimeError(payload.get("description", "Telegram API request failed"))
    return payload.get("result", [])


def chat_from_update(update):
    for key in ("message", "edited_message", "channel_post", "edited_channel_post"):
        message = update.get(key)
        if isinstance(message, dict) and isinstance(message.get("chat"), dict):
            return message["chat"]
    callback = update.get("callback_query", {})
    message = callback.get("message", {}) if isinstance(callback, dict) else {}
    return message.get("chat") if isinstance(message, dict) else None


def main():
    parser = argparse.ArgumentParser(
        description="Find chat IDs from messages sent to your Telegram bot."
    )
    parser.add_argument(
        "--wait-seconds",
        type=int,
        default=30,
        help="how long to wait for a new message (default: 30 seconds)",
    )
    args = parser.parse_args()
    if args.wait_seconds < 0:
        parser.error("--wait-seconds must be zero or greater")

    token = os.environ.get("TELEGRAM_BOT_TOKEN") or token_from_secrets_file()
    if not token:
        token = getpass.getpass("Telegram bot token (input hidden): ").strip()
    if not token or ":" not in token:
        print("No valid bot token found. Set TELEGRAM_BOT_TOKEN or configure secrets.h.", file=sys.stderr)
        return 2

    deadline = time.monotonic() + args.wait_seconds
    updates = []
    try:
        while True:
            remaining = max(0, int(deadline - time.monotonic()))
            updates = fetch_updates(token, max(1, min(10, remaining)))
            if updates or time.monotonic() >= deadline:
                break
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        if "webhook" in str(exc).lower():
            print("This bot has a webhook configured; getUpdates requires the webhook to be removed.", file=sys.stderr)
        elif "conflict" in str(exc).lower():
            print("Another process is polling this bot. Stop the ESP32 or other bot client, then retry.", file=sys.stderr)
        return 1

    chats = {}
    for update in updates:
        chat = chat_from_update(update)
        if chat and "id" in chat:
            chats[str(chat["id"])] = chat

    if not chats:
        print("No messages found. Send /start to your bot, then run this script again.")
        print("If the ESP32 is running, unplug it while retrieving the chat ID so it cannot poll the same bot.")
        return 0

    print("Chat ID(s):")
    for chat_id, chat in chats.items():
        label = chat.get("title") or chat.get("username") or chat.get("first_name") or ""
        description = f" ({label})" if label else ""
        print(f"  {chat_id} [{chat.get('type', 'unknown')}]{description}")
    print("Copy your chat ID into TELEGRAM_OWNER_CHAT_ID in secrets.h.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
