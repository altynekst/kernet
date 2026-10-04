import socket
import threading
import time
import json
import urllib.request
import urllib.error
import uuid

# Configurazione fake server
PORT = 24720
MASTER_URL = "https://master1.ddnet.org/ddnet/15/register"

SERVER_NAME = "DDNet TOOLS > t.me/aarrddnet1 <"
MAP_NAME = "t.me/aarrddnet1"
GAMETYPE = "TOOLS"
VERSION = "16.4.0"

FAKE_PLAYERS = 199
FAKE_MAX_CLIENTS = 200

def pack_str(s):
    return s.encode('utf-8') + b'\x00'

def pack_int_str(i):
    return pack_str(str(i))

def ddrace_server_info_payload(token, type_legacy=False):
    resp = bytearray()
    
    if type_legacy:
        resp.extend(b'\xff\xff\xff\xffdtsf')
    else:
        resp.extend(b'\xff\xff\xff\xffinf3')
        
    resp.extend(pack_int_str(token))
    resp.extend(pack_str(VERSION))
    resp.extend(pack_str(SERVER_NAME))
    resp.extend(pack_str(MAP_NAME))
    resp.extend(pack_str(GAMETYPE))
    resp.extend(pack_int_str(0)) # flags
    resp.extend(pack_int_str(FAKE_PLAYERS)) # players
    resp.extend(pack_int_str(FAKE_MAX_CLIENTS)) # max players
    resp.extend(pack_int_str(FAKE_PLAYERS)) # clients
    resp.extend(pack_int_str(FAKE_MAX_CLIENTS)) # max clients

    # Add clients
    for i in range(FAKE_PLAYERS):
        resp.extend(pack_str(f"aarr {i}"))
        resp.extend(pack_str("aarrddnet"))
        resp.extend(pack_int_str(-1)) # country
        resp.extend(pack_int_str(1337)) # score
        resp.extend(pack_int_str(1)) # is player
        
    return resp

def unpack_connless_packet(data):
    if len(data) >= 6 and data[:2] == b'xe':
        return True, data[2:6], data[6:]
    if len(data) >= 6 and data[:6] == b'\xff\xff\xff\xff\xff\xff':
        return False, b'\x00\x00\x00\x00', data[6:]
    return False, b'\x00\x00\x00\x00', data

def pack_connless_packet(payload, extended=False, extra=b'\x00\x00\x00\x00'):
    if extended:
        return b'xe' + (extra[:4] if extra else b'\x00\x00\x00\x00') + payload
    return b'\xff\xff\xff\xff\xff\xff' + payload

def ddrace_extended_info_payload(token):
    resp = bytearray()
    resp.extend(b'\xff\xff\xff\xffiext')
    resp.extend(pack_int_str(token))
    resp.extend(pack_str(VERSION))
    resp.extend(pack_str(SERVER_NAME))
    resp.extend(pack_str(MAP_NAME))
    resp.extend(pack_int_str(0)) # mapcrc
    resp.extend(pack_int_str(0)) # mapsize
    resp.extend(pack_str(GAMETYPE))
    resp.extend(pack_int_str(0)) # flags
    resp.extend(pack_int_str(FAKE_PLAYERS)) # players
    resp.extend(pack_int_str(FAKE_MAX_CLIENTS)) # max players
    resp.extend(pack_int_str(FAKE_PLAYERS)) # clients
    resp.extend(pack_int_str(FAKE_MAX_CLIENTS)) # max clients
    resp.extend(pack_str("")) # extra info
    
    # Can only fit enough clients for one MTU packet, usually ~40-60.
    # We will just pack 64 bots, maybe we'll truncate, but this is a POC.
    for i in range(min(FAKE_PLAYERS, 45)):
        resp.extend(pack_str(f"Bot {i}"))
        resp.extend(pack_str("FakeClan"))
        resp.extend(pack_int_str(-1))
        resp.extend(pack_int_str(1337))
        resp.extend(pack_int_str(1))
        resp.extend(pack_str("")) # Extra info player
    
    return resp

def udp_server():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", PORT))
    print(f"[UDP] Listening on port {PORT}")
    
    while True:
        try:
            data, addr = sock.recvfrom(1024)
            extended, extra, payload = unpack_connless_packet(data)

            if payload.startswith(b'\xff\xff\xff\xffgie3') and len(payload) >= 9:
                basic_token = payload[8]
                extra_token = (extra[0] << 8) | extra[1] if extended else 0
                token = basic_token | (extra_token << 8)
                resp_payload = ddrace_server_info_payload(token)
                sock.sendto(pack_connless_packet(resp_payload), addr)
                print(f"[UDP] Replied to gie3 info request from {addr}")
            
            elif payload.startswith(b'\xff\xff\xff\xfffstd') and len(payload) >= 9:
                basic_token = payload[8]
                extra_token = (extra[0] << 8) | extra[1] if extended else 0
                token = basic_token | (extra_token << 8)
                resp_payload = ddrace_server_info_payload(token, type_legacy=True)
                sock.sendto(pack_connless_packet(resp_payload), addr)
                print(f"[UDP] Replied to fstd info request from {addr}")
                
            elif (payload.startswith(b'\xff\xff\xff\xffiext') or payload.startswith(b'\xff\xff\xff\xffiex+')) and len(payload) >= 9:
                basic_token = payload[8]
                extra_token = (extra[0] << 8) | extra[1] if extended else 0
                token = basic_token | (extra_token << 8)
                resp_payload = ddrace_extended_info_payload(token)
                sock.sendto(pack_connless_packet(resp_payload), addr)
                print(f"[UDP] Replied to iext info request from {addr}")
                
            elif payload.startswith(b'\xff\xff\xff\xffchal'):
                if len(payload) > 45:
                    remainder = payload[45:].split(b'\0')
                    if len(remainder) >= 2:
                        proto = remainder[0].decode('utf-8', errors='ignore')
                        token = remainder[1].decode('utf-8', errors='ignore')
                        print(f"[UDP] Got challenge {token} for {proto}")
                        global_challenge_token[proto] = token
            else:
                pass
                
        except Exception as e:
            print(f"[UDP] Error: {e}")

global_challenge_token = {}
info_serial = 1

def master_server_registration():
    global info_serial
    secret = str(uuid.uuid4())
    challenge_secret = str(uuid.uuid4())

    while True:
        payload = {
            "max_clients": FAKE_MAX_CLIENTS,
            "max_players": FAKE_MAX_CLIENTS,
            "passworded": False,
            "game_type": GAMETYPE,
            "name": SERVER_NAME,
            "map": {
                "name": MAP_NAME,
                "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
                "size": 10000
            },
            "version": VERSION,
            "client_score_kind": "time",
            "requires_login": False,
            "clients": [{"name": f"aarr {i}", "clan": "aarrddnet", "country": -1, "score": 1337, "is_player": True} for i in range(min(FAKE_PLAYERS, FAKE_MAX_CLIENTS))]
        }
        
        proto = "tw0.6/ipv4"
        addr_proto = "tw-0.6+udp://"
        
        req_headers = {
            'Content-Type': 'application/json',
            'Address': f'{addr_proto}connecting-address.invalid:{PORT}',
            'Secret': secret,
            'Challenge-Secret': f'{challenge_secret}:{proto}',
            'Info-Serial': str(info_serial),
            'User-Agent': f'Teeworlds DDNet {VERSION}'
        }
        
        if proto in global_challenge_token:
            req_headers['Challenge-Token'] = global_challenge_token[proto]

        req = urllib.request.Request(MASTER_URL, data=json.dumps(payload).encode('utf-8'), headers=req_headers)
        
        try:
            with urllib.request.urlopen(req) as response:
                resp_str = response.read().decode('utf-8')
                resp_json = json.loads(resp_str) if resp_str else {}
                status = resp_json.get("status", "success")
                if status == "success":
                    print(f"[MASTER] Registered successfully to {proto}")
                elif status == "need_challenge":
                    # Pending UDP challenge
                    pass
                elif status == "need_info":
                    info_serial += 1
        except urllib.error.HTTPError as e:
            err_body = e.read().decode('utf-8')
            print(f"[MASTER] Failed to register: HTTP {e.code} - {err_body}")
        except Exception as e:
            print(f"[MASTER] Failed to register: {e}")
            
        time.sleep(15)

if __name__ == "__main__":
    t1 = threading.Thread(target=udp_server, daemon=True)
    t2 = threading.Thread(target=master_server_registration, daemon=True)
    t1.start()
    t2.start()
    
    print("Fake server is running... Press Ctrl+C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("Stopping.")