#!/usr/bin/env python3
"""
Web bridge for the auction system.
Connects to the C server as a client and exposes a web interface.

Usage: python3 web/server.py [auction_ip] [auction_port] [web_port] [room]
  Default: python3 web/server.py 127.0.0.1 8080 5000 1
"""

import socket
import struct
import threading
import json
import sys
import time
import os
from http.server import HTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

# Protocol constants (must match common.h)
MAX_NAME_LEN = 32
MAX_ITEM_LEN = 64
BUFFER_SIZE  = 256

MSG_JOIN         = 1
MSG_BID          = 2
MSG_AUCTION_INFO = 3
MSG_BID_OK       = 4
MSG_BID_REJECT   = 5
MSG_UPDATE       = 6
MSG_TIMER        = 7
MSG_WINNER       = 8
MSG_CHAT         = 9
MSG_BYE          = 10
MSG_CONNECTED    = 11
MSG_HISTORY      = 12
MSG_BALANCE      = 13
MSG_NEXT_AUCTION = 14

# Message struct layout: uint8 + 32s + 64s + uint32 + uint32 + uint8 + 256s
MSG_FORMAT = '<B32s64sIIB256s'
MSG_SIZE   = struct.calcsize(MSG_FORMAT)

# Shared state
state = {
    "item": "",
    "current_price": 0,
    "leader": "aucun",
    "time_left": 60,
    "connected": 0,
    "history": [],
    "messages": [],
    "winner": None,
    "balance": 1000,
    "room": 0
}
state_lock = threading.Lock()
sock = None


def decode_msg(data):
    if len(data) < MSG_SIZE:
        return None
    parts = struct.unpack(MSG_FORMAT, data[:MSG_SIZE])
    return {
        "type": parts[0],
        "name": parts[1].split(b'\x00')[0].decode('utf-8', errors='replace'),
        "item": parts[2].split(b'\x00')[0].decode('utf-8', errors='replace'),
        "amount": parts[3],
        "balance": parts[4],
        "room": parts[5],
        "text": parts[6].split(b'\x00')[0].decode('utf-8', errors='replace'),
    }


def encode_msg(msg_type, name="", item="", amount=0, balance=0, room=0, text=""):
    return struct.pack(MSG_FORMAT,
                       msg_type,
                       name.encode('utf-8')[:MAX_NAME_LEN].ljust(MAX_NAME_LEN, b'\x00'),
                       item.encode('utf-8')[:MAX_ITEM_LEN].ljust(MAX_ITEM_LEN, b'\x00'),
                       amount,
                       balance,
                       room,
                       text.encode('utf-8')[:BUFFER_SIZE].ljust(BUFFER_SIZE, b'\x00'))


def receiver_thread():
    global sock
    while True:
        try:
            data = sock.recv(MSG_SIZE)
            if not data:
                break
            msg = decode_msg(data)
            if not msg:
                continue

            with state_lock:
                if msg["type"] == MSG_AUCTION_INFO:
                    state["item"] = msg["item"]
                    state["current_price"] = msg["amount"]
                    state["messages"].append({"type": "info", "text": msg["text"]})
                elif msg["type"] == MSG_UPDATE:
                    state["current_price"] = msg["amount"]
                    state["leader"] = msg["name"]
                    state["history"].append({
                        "time": time.strftime("%H:%M:%S"),
                        "bidder": msg["name"],
                        "amount": msg["amount"]
                    })
                    state["messages"].append({"type": "bid", "text": msg["text"]})
                elif msg["type"] == MSG_TIMER:
                    state["time_left"] = msg["amount"]
                elif msg["type"] == MSG_WINNER:
                    state["winner"] = {"name": msg["name"], "amount": msg["amount"], "text": msg["text"]}
                    state["messages"].append({"type": "winner", "text": msg["text"]})
                elif msg["type"] == MSG_CONNECTED:
                    state["connected"] = msg["amount"]
                elif msg["type"] == MSG_BALANCE:
                    state["balance"] = msg["balance"]
                elif msg["type"] == MSG_BID_OK:
                    state["messages"].append({"type": "ok", "text": msg["text"]})
                elif msg["type"] == MSG_BID_REJECT:
                    state["messages"].append({"type": "error", "text": msg["text"]})
                elif msg["type"] == MSG_CHAT:
                    state["messages"].append({"type": "chat", "text": msg["text"]})
                elif msg["type"] == MSG_NEXT_AUCTION:
                    state["winner"] = None
                    state["history"] = []
                    state["messages"].append({"type": "next", "text": msg["text"]})
                elif msg["type"] == MSG_HISTORY:
                    state["messages"].append({"type": "history", "text": msg["text"]})

                if len(state["messages"]) > 50:
                    state["messages"] = state["messages"][-50:]

        except Exception as e:
            print(f"[WEB] Receiver error: {e}")
            break


class AuctionHandler(SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path == '/api/state':
            with state_lock:
                resp = json.dumps(state)
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(resp.encode())

        elif parsed.path == '/api/bid':
            params = parse_qs(parsed.query)
            amount = int(params.get('amount', [0])[0])
            if amount > 0 and sock:
                data = encode_msg(MSG_BID, amount=amount, room=state["room"])
                sock.send(data)
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(b'{"ok":true}')
            else:
                self.send_response(400)
                self.end_headers()
                self.wfile.write(b'{"ok":false}')

        elif parsed.path == '/' or parsed.path == '/index.html':
            html_path = os.path.join(os.path.dirname(__file__), 'index.html')
            with open(html_path, 'rb') as f:
                content = f.read()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.end_headers()
            self.wfile.write(content)

        else:
            self.send_response(404)
            self.end_headers()


def main():
    global sock

    auction_ip   = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    auction_port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    web_port     = int(sys.argv[3]) if len(sys.argv) > 3 else 5000
    room         = int(sys.argv[4]) - 1 if len(sys.argv) > 4 else 0

    state["room"] = room
    bot_name = f"Web_{int(time.time()) % 1000}"

    print(f"[WEB] Connexion au serveur d'encheres {auction_ip}:{auction_port} (salle {room+1})")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((auction_ip, auction_port))

    join_msg = encode_msg(MSG_JOIN, name=bot_name, room=room)
    sock.send(join_msg)

    print(f"[WEB] Connecte comme '{bot_name}'")

    t = threading.Thread(target=receiver_thread, daemon=True)
    t.start()

    server = HTTPServer(('0.0.0.0', web_port), AuctionHandler)
    print(f"[WEB] Interface web sur http://0.0.0.0:{web_port}")
    print(f"[WEB] Ouvrez dans le navigateur : http://localhost:{web_port}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[WEB] Arret.")
        sock.close()


if __name__ == "__main__":
    main()
