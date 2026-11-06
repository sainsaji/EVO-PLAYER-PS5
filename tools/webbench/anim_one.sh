#!/bin/bash
# Serve third_party/ProsperoWeb/tools/bench on the PC first: (cd third_party/ProsperoWeb/tools/bench && python3 -m http.server 8123 --bind 0.0.0.0). The PC address (192.168.0.13) is hard-coded below.
# usage: anim_one.sh <bg|canvas|shadow|spin|text>
# Opens tools/bench/t_<kind>.html in the Web section (page must be reachable at
# the PC's http server, port 8123), takes 3 screenshots 1 s apart and prints how
# much of the animated box changed between them.
cd "$(dirname "$0")/../.."
K=$1
cp third_party/ProsperoWeb/tools/bench/animtest.html third_party/ProsperoWeb/tools/bench/t_$K.html
./tools/dx.sh ./tools/evo-remote.sh weburl "http://192.168.0.13:8123/t_$K.html" >/dev/null 2>&1
sleep 6
rm -f output/anim/one_*
for i in 1 2 3; do ./tools/dx.sh ./tools/evo-remote.sh key l3 >/dev/null 2>&1; ./tools/shot.sh grab >/dev/null 2>&1; cp output/screenshots/latest.png output/anim/one_$i.png; sleep 1; done
python3 -W ignore -I -c "
from PIL import Image, ImageChops
imgs=[Image.open(f'output/anim/one_{i}.png').convert('RGB') for i in (1,2,3)]
box=(440,285,915,560)
out=[]
for a,b in zip(imgs,imgs[1:]):
    d=ImageChops.difference(a.crop(box),b.crop(box)); px=list(d.getdata())
    out.append(round(100*sum(1 for p in px if max(p)>24)/len(px),1))
print('$K', 'changed % in the animated box between shots:', out)
"
