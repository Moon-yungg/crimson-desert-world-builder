# In-game spawn test over the World Builder HTTP API. Run it ONLY when asked for an in-game test: it can start the game,
# presses keys in its window and takes screenshots of it (never of other windows).
#
#   python scripts\ingame_test.py prefabs.txt              use the running game (player in the world), spawn each prefab alone
#   python scripts\ingame_test.py prefabs.txt --launch     start the game via Steam, continue the last save with "E", then test
#   python scripts\ingame_test.py --status                 print /api/status, /api/player, /api/camera and exit
#
# prefabs.txt: one logical prefab path per line ("/object/.../x.prefab"), "#" comments allowed.
# Every prefab is spawned 4 m ahead and 3.5 m beside the character along the camera view (small props hide behind the
# character's head otherwise), screenshotted, and deleted again. Result: <out>/sheet.png (crops) + result.json.
# Requires settings.txt http_api=1. Keyboard input works, mouse input does not reach the game (raw input ignores it),
# so the camera is never turned: the objects are placed where the camera already looks.
import argparse, ctypes, json, os, subprocess, sys, time, urllib.request
from ctypes import wintypes

API = "http://127.0.0.1:8765"
GAME_LOG = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\cdmodkit\cdmodkit.log"
user32 = ctypes.windll.user32
user32.SetProcessDPIAware()   # the desktop runs at 125 %: without this the capture misses the right fifth

def call(method, path, body=None, timeout=5):
    req = urllib.request.Request(API + path, data=json.dumps(body).encode() if body is not None else None, method=method,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, json.loads(r.read() or b"{}")

def game_window():
    return user32.FindWindowW(None, "Crimson Desert")

def focus_game():
    h = game_window()
    if not h: return False
    user32.ShowWindow(h, 9)
    user32.keybd_event(0x12, 0, 0, 0); user32.keybd_event(0x12, 0, 2, 0)   # an ALT tap lets a background process take the foreground
    user32.SetForegroundWindow(h); time.sleep(0.5)
    return user32.GetForegroundWindow() == h

class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]
class INPUT(ctypes.Structure):
    class _U(ctypes.Union): _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",); _fields_ = [("type", wintypes.DWORD), ("u", _U)]

def press(scan, hold=0.12):   # by scan code: the game reads keys by scan code
    for up in (False, True):
        i = INPUT(type=1); i.ki = KEYBDINPUT(0, scan, 0x0008 | (0x0002 if up else 0), 0, 0)
        user32.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))
        if not up: time.sleep(hold)

def screenshot(path):
    """Only the game: returns None when its window is not in front."""
    from PIL import ImageGrab
    if not focus_game(): return None
    im = ImageGrab.grab(all_screens=False); im = im.resize((1280, int(im.height * 1280 / im.width)))
    im.save(path); return im

def wait(cond, seconds, step=2.0):
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(step)
    return False

def in_world():
    _, s = call("GET", "/api/status", timeout=3); st, p = call("GET", "/api/player", timeout=3)
    return s.get("ready") and st == 200 and (abs(p["x"]) > 1 or abs(p["z"]) > 1)   # (0, 1000, 0) while loading

def launch_and_continue():
    if game_window(): print("game already running, not launching"); return
    start = os.path.getsize(GAME_LOG) if os.path.exists(GAME_LOG) else 0
    os.startfile("steam://rungameid/3321460"); print("launching ...")
    def overlay_ready():
        with open(GAME_LOG, "rb") as f: f.seek(start if os.path.getsize(GAME_LOG) >= start else 0); return b"[overlay] ready" in f.read()
    if not wait(overlay_ready, 300): sys.exit("overlay never became ready")
    time.sleep(12)   # title screen
    if not focus_game(): sys.exit("could not bring the game to the front")
    press(0x12)   # E = "Spielen" (continue). Never Z (new game) or ESC (quit) on this screen
    print("continue pressed, loading ...")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefabs", nargs="?"); ap.add_argument("--launch", action="store_true"); ap.add_argument("--status", action="store_true")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "cdk_ingame_test"))
    a = ap.parse_args()
    if a.status:
        for p in ("/api/status", "/api/player", "/api/camera"):
            try: print(p, call("GET", p))
            except Exception as e: print(p, e)
        return
    if not a.prefabs: ap.error("prefab list missing")
    prefabs = [l.strip() for l in open(a.prefabs, encoding="utf-8") if l.strip() and not l.startswith("#")]
    if a.launch: launch_and_continue()
    if not wait(in_world, 900): sys.exit("player not in the world (is the game running with http_api=1?)")
    time.sleep(6)
    os.makedirs(a.out, exist_ok=True)
    _, P = call("GET", "/api/player"); _, C = call("GET", "/api/camera")
    v = C.get("view") or {"x": 0, "z": 1}; side = (v["z"], -v["x"])
    spot = (P["x"] + v["x"] * 4 + side[0] * 3.5, P["y"], P["z"] + v["z"] * 4 + side[1] * 3.5)
    def settle(extra):
        wait(lambda: not call("GET", "/api/status")[1].get("pending"), 8, 0.2); time.sleep(extra)
    shots = [("empty", screenshot(os.path.join(a.out, "empty.png")))]; results = []
    for i, prefab in enumerate(prefabs):
        st, r = call("POST", "/api/objects", {"prefab": prefab, "x": spot[0], "y": spot[1], "z": spot[2], "yaw": 0, "scale": 1})
        settle(2.5); name = prefab.rsplit("/", 1)[-1].replace(".prefab", "")
        shots.append((name, screenshot(os.path.join(a.out, f"{i:02d}_{name}.png"))))
        uid = r.get("uid"); results.append({"prefab": prefab, "status": st, "uid": uid})
        if uid: call("DELETE", f"/api/objects/{uid}")
        settle(1.0); print(f"{st} {name}")
    from PIL import Image, ImageDraw
    box = (380, 250, 1100, 650); T = (box[2] - box[0], box[3] - box[1]); cols = 3; rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (T[0] + 8) + 8, rows * (T[1] + 26) + 8), (30, 32, 40)); d = ImageDraw.Draw(sheet)
    for k, (name, im) in enumerate(shots):
        x = 8 + (k % cols) * (T[0] + 8); y = 8 + (k // cols) * (T[1] + 26); d.text((x + 2, y + 4), name, fill=(235, 235, 240))
        if im: sheet.paste(im.crop(box), (x, y + 22))
        else: d.text((x + 10, y + 100), "no screenshot: game window was not in front", fill=(230, 120, 120))
    sheet.save(os.path.join(a.out, "sheet.png"))
    json.dump({"player": P, "camera": C, "spot": spot, "results": results}, open(os.path.join(a.out, "result.json"), "w"), indent=1)
    print("sheet:", os.path.join(a.out, "sheet.png"))

if __name__ == "__main__":
    main()
