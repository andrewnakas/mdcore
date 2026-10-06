#!/bin/sh
# Solos each sound channel on both cores and compares them numerically.
#   audio_solo.sh ROM SCRIPT FRAMES SKIP_SECONDS
cd "$(dirname "$0")"
ROM=$1; SC=$2; FR=$3; SK=${4:-0}
CORE=oracle/gpgx/genesis_plus_gx_libretro.so
ALLFM="genesis_plus_gx_show_advanced_audio_settings=enabled;genesis_plus_gx_ym2612=mame (ym2612)"
solo() { # $1 name, $2 mdcore mask, $3 reference options
    TRACE_SOUNDMASK=$2 vectors/trace "$ROM" "$FR" "$SC" vectors/s_md.raw >/dev/null
    ORACLE_OPT="$ALLFM;$3" vectors/oracle $CORE "$ROM" "$FR" "$SC" vectors/s_gx.raw >/dev/null
    echo "== $1"; vectors/audio_compare vectors/s_md.raw 53267 vectors/s_gx.raw 44100 "$SK" | sed 's/^/   /'
}
fmoff() { o=""; for c in 0 1 2 3 4 5; do [ "$c" = "$1" ] || o="$o;genesis_plus_gx_md_channel_${c}_volume=0"; done; echo "$o"; }
psgoff="genesis_plus_gx_psg_channel_0_volume=0;genesis_plus_gx_psg_channel_1_volume=0;genesis_plus_gx_psg_channel_2_volume=0;genesis_plus_gx_psg_channel_3_volume=0"
for c in 0 1 2 3 4 5; do
    m=$(printf '%x' $(( 0x3FF & ~(1 << c) )))
    solo "FM channel $((c+1))" $m "$(fmoff $c);$psgoff"
done
fmall="$(fmoff 9)"
for c in 0 1 2 3; do
    m=$(printf '%x' $(( 0x3FF & ~(0x40 << c) )))
    o=""; for k in 0 1 2 3; do [ $k = $c ] || o="$o;genesis_plus_gx_psg_channel_${k}_volume=0"; done
    solo "PSG channel $((c+1))" $m "$fmall$o"
done
