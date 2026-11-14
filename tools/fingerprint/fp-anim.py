#!/usr/bin/env python3
# fp-anim.py: animated live view of the fingerprint enroll loop (kern.redwood.tee "fp run 3 ...") from the phone's
# kernel log. White = press, green = finger on the sensor, red = rejected (white again when ready to retry).
import re, subprocess, sys, time

URL = 'http://10.77.0.2:8080/tail?log=0&dmesg=0'
ART = r'''
           .-~~~~~~~~~-.
        .-'  .-~~~~~-.  '-.
      .'   .'  .---.  '.   '.
     /   .'  .'     '.  '.   \
    /   /   /  .---.  \   \   \
   |   |   |  /     \  |   |   |
   |   |   |  | (*) |  |   |   |
   |   |   |  \     /  |   |   |
   |   |    \  '---'  /    |   |
    \   \    '.     .'    /   /
     \   '.    '---'    .'   /
      '.   '-.       .-'   .'
        '-.   '~---~'   .-'
           '-.._____..-'
'''.strip('\n').split('\n')
COL = {'white': '\033[1;97m', 'green': '\033[1;92m', 'red': '\033[1;91m', 'grey': '\033[90m'}
TXT = {'match': 'MATCH  -- that is you', 'nomatch': 'NO MATCH -- not this finger',
       'white': 'PRESS the sensor', 'green': 'HOLD...  reading your finger', 'red': 'NOT GOOD -- lift your finger',
       'grey': 'waiting for the phone...', 'done': 'ENROLLED  -- all done'}
RST = '\033[0m'
LINE = re.compile(r'^\[ *([0-9.]+)\]: redwood-tee: (fp run .*)$')

state, progress, good, bad, red_until, last_t = 'grey', 0, 0, 0, 0.0, -1.0
msg = ''


def draw():
    c = {'done': 'green', 'match': 'green', 'nomatch': 'red'}.get(state, state)
    out = ['\033[H\033[2J', '\n']
    for l in ART:
        out.append('      ' + COL[c] + l + RST + '\n')
    bar = int(progress * 30 / 100)
    out.append('\n      ' + COL[c] + TXT[state].center(34) + RST + '\n\n')
    out.append('      [' + COL['green'] + '#' * bar + RST + '.' * (30 - bar) + '] %3d%%\n' % progress)
    out.append('      good %d   retries %d\n' % (good, bad))
    if msg:
        out.append('\n      ' + COL['grey'] + msg + RST + '\n')
    sys.stdout.write(''.join(out))
    sys.stdout.flush()


def event(text):
    global state, progress, good, bad, red_until, msg
    now = time.time()
    if 'fp run 6: MATCH' in text:
        good += 1
        state, red_until, msg = 'match', now + 2.0, 'lift; press again to test'
        return
    if 'fp run 6: NO MATCH' in text or ('fp run 6: bad image (0x20)' in text):
        bad += 1
        state, red_until, msg = 'nomatch', now + 2.0, 'lift; press again'
        return
    if 'SAMPLE' in text and 'ACCEPTED' in text:
        good += 1
        m = re.search(r'progress (\d+)', text)
        progress = int(m.group(1)) if m else progress
        state = 'done' if progress >= 100 else 'green'
        msg = 'good one -- now LIFT your finger'
    elif 'error 0x27' in text or 'capture rejected' in text or 'bad image' in text:
        if state != 'red':
            bad += 1
        state, red_until = 'red', now + 1.2
        msg = 'lift, move a little, press again (flat and steady)'
        m = re.search(r'quality ([0-9a-f]{8})', text)
        if m:
            q = int(m.group(1), 16)
            # libbauthserver finger_image_quality_map
            for bit, why in ((0x08000000, 'only part of your finger: cover the whole spot'),
                             (0x04000000, 'too light: press a little harder'),
                             (0x01000000, 'finger too wet: dry it'),
                             (0x2000, 'screen dirty: wipe the spot'),
                             (0x8, 'same as last time: lift and move your finger a little'),
                             (0x2, 'too fast: hold until it turns green'),
                             (0x100, 'too slow')):
                if q & bit:
                    msg = why
                    break
    elif 'ON THE SENSOR' in text or 'finger held' in text:
        if now >= red_until:
            state, msg = 'green', ''
    elif 'touch at' in text:
        if now >= red_until:
            state, msg = 'white', 'off the spot: centred, about 3 cm up from the bottom edge'
    elif 'finger lifted' in text or ('-> result 0' in text and re.search(r' op 4 error 0 ', text)):
        if state != 'done':
            state, msg = ('white', '') if now >= red_until else (state, msg)
    elif 'steps, last op' in text:
        if state != 'done':
            state, msg = 'grey', 'run ended'


fails = 0
draw()
while True:
    try:
        data = subprocess.run(['curl', '-s', '-m', '3', URL], capture_output=True, timeout=6).stdout.decode('latin1', 'replace')
        if not data:
            raise OSError('no data')
    except Exception:
        fails += 1
        if fails >= 3:                       # rwlogd serves one connection and sometimes wedges: restart it
            subprocess.run(['ssh', '-o', 'ConnectTimeout=5', 's20',
                            'kill $(ps -A -o pid,comm | grep rwlogd | cut -c1-6)'], capture_output=True, timeout=15)
            fails = 0
            time.sleep(3)
        time.sleep(1)
        continue
    fails = 0
    for l in data.split('\n'):
        m = LINE.match(l)
        if not m:
            continue
        t = float(m.group(1))
        if t < last_t - 100:
            last_t = -1.0                                   # the phone rebooted
        if t <= last_t:
            continue
        last_t = t
        event(m.group(2))
    if state in ('red', 'match', 'nomatch') and time.time() >= red_until:
        state, msg = 'white', 'try again: press the sensor'
    draw()
    time.sleep(1.0)
