#!/bin/zsh
# fp-watch.sh: live view of the fingerprint enroll loop (kern.redwood.tee "fp run ...") from the phone's kernel log
seen=0
print "fingerprint live view -- press ~3 cm up from the bottom edge, centred\n"
while true; do
  curl -s -m 5 'http://10.77.0.2:8080/tail?log=0&dmesg=0' | grep -aE '^\[ *[0-9.]+\]: redwood-tee: fp run' > /tmp/fpw.$$ 
  while IFS= read -r l; do
    t=${${l#\[}%%\]*}; t=${t// /}
    (( ${t%.*}${t#*.} <= seen )) && continue
    seen=${t%.*}${t#*.}
    case $l in
      *"touch at"*"ON THE SENSOR"*) xy=${${l#*touch at }%%:*}; y=${xy#*,}
         if (( y < 49500 )); then hint="(a bit LOWER is better)"; elif (( y > 54000 )); then hint="(a bit HIGHER)"; else hint="(good spot)"; fi
         print -P "%F{cyan}touch on sensor  $xy $hint%f";;
      *"touch at"*) print -P "%F{yellow}touch OFF the sensor  ${${l#*touch at }%%:*}  -- move to the centre, ~3 cm up%f";;
      *"finger held"*) print -P "%F{white}%BHOLD... capturing%b%f";;
      *"SAMPLE"*"ACCEPTED"*) n=${${l#*SAMPLE }%% *}; p=${${l#*progress }%%;*}
         print -P "%F{green}%B GOOD  sample $n  progress $p%%  -> LIFT, then press again%b%f";;
      *"steps, last op"*) print -P "%F{magenta}round finished: ${l#*fp run 3: }%f";;
      *"bad image"*) bad=$(( bad + 1 )); print -P "%F{red}%B BAD image ($bad) -> lift, press again (flatter / steadier)%b%f";;
      *"step 1: op 1"*) print -P "%F{white}%BPRESS NOW%b%f";;
      *"enroll-final op"*) print -P "%F{white}  (sample merged: ${l#*-> })%f";;
    esac
  done < /tmp/fpw.$$
  sleep 0.5
done
