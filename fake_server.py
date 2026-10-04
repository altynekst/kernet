import socket
import threading
import time
import json
import urllib.request
import urllib.error
import uuid

# --- Конфигурация: добавь сколько нужно серверов ---
# Пример: два сервера с разными портами/именами
SERVERS = [
    {
        "PORT": 24720,
        "MASTER_URL": "https://master1.ddnet.org/ddnet/15/register",
        "SERVER_NAME": "DDNet TOOLS > t.me/aarrddnet1 <",
        "MAP_NAME": "t.me/aarrddnet1",
        "GAMETYPE": "TOOLS",
        "VERSION": "16.4.0",
        "FAKE_PLAYERS": 199,
        "FAKE_MAX_CLIENTS": 200,
    },
    {
        "PORT": 24721,  # второй сервер на другом порту
        "MASTER_URL": "https://master1.ddnet.org/ddnet/15/register",
        "SERVER_NAME": "DDNet TOOLS 2 > t.me/aarrddnet1 <",
        "MAP_NAME": "t.me/aarrddnet1_second",
        "GAMETYPE": "TOOLS",
        "VERSION": "16.4.0",
        "FAKE_PLAYERS": 50,
        "FAKE_MAX_CLIENTS": 128,
    },
]
# -----------------------------------------------------

def pack_str(s):
    return s.encode('utf-8') + b'\x00'

def pack_int_str(i):
    return pack_str(str(i))

def unpack_connless_packet(data):
    # Возвращает (extended_flag:bool, extra_bytes:bytes(4), payload:bytes)
    if len(data) >= 6 and data[:2] == b'xe':
        return True, data[2:6], data[6:]
    if len(data) >= 6 and data[:6] == b'\xff\xff\xff\xff\xff\xff':
        return False, b'\x00\x00\x00\x00', data[6:]
    return False, b'\x00\x00\x00\x00', data

def pack_connless_packet(payload, extended=False, extra=b'\x00\x00\x00\x00'):
    if extended:
        return b'xe' + (extra[:4] if extra else b'\x00\x00\x00\x00') + payload
    return b'\xff\xff\xff\xff\xff\xff' + payload

def ddrace_server_info_payload(cfg, token, type_legacy=False):
    VERSION = cfg["VERSION"]
    SERVER_NAME = cfg["SERVER_NAME"]
    MAP_NAME = cfg["MAP_NAME"]
    GAMETYPE = cfg["GAMETYPE"]
    FAKE_PLAYERS = cfg["FAKE_PLAYERS"]
    FAKE_MAX_CLIENTS = cfg["FAKE_MAX_CLIENTS"]

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

    # Add clients (ограничить, чтобы не делать слишком большой пакет)
    for i in range(min(FAKE_PLAYERS, FAKE_MAX_CLIENTS)):
        resp.extend(pack_str(f"aarr {i}"))
        resp.extend(pack_str("aarrddnet"))
        resp.extend(pack_int_str(-1)) # country
        resp.extend(pack_int_str(1337)) # score
        resp.extend(pack_int_str(1)) # is player

    return resp

def ddrace_extended_info_payload(cfg, token):
    VERSION = cfg["VERSION"]
    SERVER_NAME = cfg["SERVER_NAME"]
    MAP_NAME = cfg["MAP_NAME"]
    GAMETYPE = cfg["GAMETYPE"]
    FAKE_PLAYERS = cfg["FAKE_PLAYERS"]
    FAKE_MAX_CLIENTS = cfg["FAKE_MAX_CLIENTS"]

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

    # Пакуем ограниченное число ботов (чтобы не превысить MTU)
    for i in range(min(FAKE_PLAYERS, 45)):
        resp.extend(pack_str(f"Bot {i}"))
        resp.extend(pack_str("FakeClan"))
        resp.extend(pack_int_str(-1))
        resp.extend(pack_int_str(1337))
        resp.extend(pack_int_str(1))
        resp.extend(pack_str("")) # Extra info player

    return resp

def udp_server_thread(cfg, state):
    """
    state: dict — отдельный словарь состояния для сервера (e.g. global_challenge_token)
    """
    PORT = cfg["PORT"]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", PORT))
    print(f"[UDP][{PORT}] Listening on port {PORT}")

    while True:
        try:
            data, addr = sock.recvfrom(4096)
            extended, extra, payload = unpack_connless_packet(data)

            # gie3/info request
            if payload.startswith(b'\xff\xff\xff\xffgie3') and len(payload) >= 9:
                basic_token = payload[8]
                extra_token = (extra[0] << 8) | extra[1] if extended else 0
                token = basic_token | (extra_token << 8)
                resp_payload = ddrace_server_info_payload(cfg, token)
                sock.sendto(pack_connless_packet(resp_payload), addr)
                print(f"[UDP][{PORT}] Replied to gie3 info request from {addr}")

            # legacy fstd
            elif payload.startswith(b'\xff\xff\xff\xfffstd') and len(payload) >= 9:
                basic_token = payload[8]
                extra_token = (extra[0] << 8) | extra[1] if extended else 0
                token = basic_token | (extra_token << 8)
                resp_payload = ddrace_server_info_payload(cfg, token, type_legacy=True)
                sock.sendto(pack_connless_packet(resp_payload), addr)
                print(f"[UDP][{PORT}] Replied to fstd info request from {addr}")

            # iext / iex+ extended info
            elif (payload.startswith(b'\xff\xff\xff\xffiext') or payload.startswith(b'\xff\xff\xff\xffiex+')) and len(payload) >= 9:
                basic_token = payload[8]
                extra_token = (extra[0] << 8) | extra[1] if extended else 0
                token = basic_token | (extra_token << 8)
                resp_payload = ddrace_extended_info_payload(cfg, token)
                sock.sendto(pack_connless_packet(resp_payload), addr)
                print(f"[UDP][{PORT}] Replied to iext info request from {addr}")

            # challenge packet (chal)
            elif payload.startswith(b'\xff\xff\xff\xffchal'):
                if len(payload) > 45:
                    remainder = payload[45:].split(b'\0')
                    if len(remainder) >= 2:
                        proto = remainder[0].decode('utf-8', errors='ignore')
                        token = remainder[1].decode('utf-8', errors='ignore')
                        print(f"[UDP][{PORT}] Got challenge {token} for {proto} from {addr}")
                        state['global_challenge_token'][proto] = token
            else:
                # можно логировать непонятные пакеты при отладке
                pass

        except Exception as e:
            print(f"[UDP][{PORT}] Error: {e}")

def master_server_registration_thread(cfg, state):
    PORT = cfg["PORT"]
    MASTER_URL = cfg["MASTER_URL"]
    FAKE_PLAYERS = cfg["FAKE_PLAYERS"]
    FAKE_MAX_CLIENTS = cfg["FAKE_MAX_CLIENTS"]
    GAMETYPE = cfg["GAMETYPE"]
    SERVER_NAME = cfg["SERVER_NAME"]
    MAP_NAME = cfg["MAP_NAME"]
    VERSION = cfg["VERSION"]

    info_serial = 1
    secret = str(uuid.uuid4())
    challenge_secret = str(uuid.uuid4())

    proto = "tw0.6/ipv4"
    addr_proto = "tw-0.6+udp://"

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

        req_headers = {
            'Content-Type': 'application/json',
            'Address': f'{addr_proto}connecting-address.invalid:{PORT}',
            'Secret': secret,
            'Challenge-Secret': f'{challenge_secret}:{proto}',
            'Info-Serial': str(info_serial),
            'User-Agent': f'Teeworlds DDNet {VERSION}'
        }

        # Подставляем challenge token, если он пришёл через UDP (chal)
        if proto in state['global_challenge_token']:
            req_headers['Challenge-Token'] = state['global_challenge_token'][proto]

        req = urllib.request.Request(MASTER_URL, data=json.dumps(payload).encode('utf-8'), headers=req_headers)

        try:
            with urllib.request.urlopen(req, timeout=8) as response:
                resp_str = response.read().decode('utf-8')
                resp_json = json.loads(resp_str) if resp_str else {}
                status = resp_json.get("status", "success")
                if status == "success":
                    print(f"[MASTER][{PORT}] Registered successfully to {proto}")
                elif status == "need_challenge":
                    print(f"[MASTER][{PORT}] Master requests UDP challenge")
                    # ждём, udp_server_thread получит chal и сохранит токен в state
                elif status == "need_info":
                    info_serial += 1
                    print(f"[MASTER][{PORT}] Master requested info serial increment, now {info_serial}")
        except urllib.error.HTTPError as e:
            try:
                err_body = e.read().decode('utf-8')
            except Exception:
                err_body = "<no body>"
            print(f"[MASTER][{PORT}] Failed to register: HTTP {e.code} - {err_body}")
        except Exception as e:
            print(f"[MASTER][{PORT}] Failed to register: {e}")

        time.sleep(15)

def start_server_instance(cfg):
    """
    Создаёт state для инстанса и запускает UDP и MASTER потоки.
    """
    state = {
        'global_challenge_token': {}  # отдельно для каждого инстанса
    }

    t_udp = threading.Thread(target=udp_server_thread, args=(cfg, state), daemon=True)
    t_master = threading.Thread(target=master_server_registration_thread, args=(cfg, state), daemon=True)
    t_udp.start()
    t_master.start()
    return (t_udp, t_master)

if __name__ == "__main__":
    threads = []
    for cfg in SERVERS:
        threads.append(start_server_instance(cfg))

    print("Fake servers are running... Press Ctrl+C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("Stopping all servers.")
        # Демонпотоки завершаются при выходе программы