#!/usr/bin/env bash
# tools/e2e/run-e2e.sh — регрессионный стенд привязки вывода к окну ([output] pin_window).
#
# Поднимает Xvfb + openbox (если X-сервер ещё не запущен), открывает тестовые
# Qt-окна (probe) с QLineEdit и перехватом сырых xcb-событий — видно,
# настоящими событиями (XTest/клавиатура) пришёл ввод или синтетикой
# (XSendEvent, флаг send_event=1), — и прогоняет `voice-assistant --type`
# во всех комбинациях pin_mode / method / restore_focus.
#
# Запуск (из любого места):
#   bash tools/e2e/run-e2e.sh
#
# Переменные окружения:
#   VOICE_ASSISTANT_BIN  путь к бинарнику        (по умолчанию <repo>/build/src/voice-assistant)
#   E2E_DISPLAY          дисплей X                (по умолчанию :99)
#   E2E_KEEP_X=1         не гасить поднятый стендом Xvfb/openbox при выходе
#
# Требования: Xvfb, openbox, xdotool, xclip, x11-utils (xdpyinfo, xprop),
#             x11-xkb-utils (setxkbmap, желательно), Qt5 (для сборки probe),
#             собранный проект (build/src/voice-assistant).
#
# В ctest НЕ регистрируется: стенду нужен X, поэтому запуск вручную.
# Норма: 13 сценариев, все проверки зелёные.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

BIN="${VOICE_ASSISTANT_BIN:-$REPO/build/src/voice-assistant}"
PROBE="$REPO/build/tools/e2e/probe"
D="${TMPDIR:-/tmp}/voice-assistant-e2e"
export DISPLAY="${E2E_DISPLAY:-:99}"

PASS=0; FAIL=0; SCEN=0
XVFB_STARTED=0; OPENBOX_STARTED=0

say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); say "    [ok]   $1"; }
bad()  { FAIL=$((FAIL+1)); say "    [FAIL] $1"; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: получили «$2», ждали «$3»"; fi; }
has()  { if grep -q "$2" "$1"; then ok "лог: $2"; else bad "в логе нет «$2»"; fi; }
scen() { SCEN=$((SCEN+1)); say ""; say "== Сценарий $SCEN: $1"; }
die()  { say "ОШИБКА: $*"; exit 1; }

cleanup() {
    pkill -x probe 2>/dev/null
    if [ "${E2E_KEEP_X:-0}" != "1" ]; then
        [ "$OPENBOX_STARTED" = "1" ] && pkill -x openbox 2>/dev/null
        [ "$XVFB_STARTED" = "1" ] && pkill -x Xvfb 2>/dev/null
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- зависимости
for t in xdotool xclip xdpyinfo xprop; do
    command -v "$t" >/dev/null || die "нет $t (sudo apt install xdotool xclip x11-utils)"
done
[ -x "$BIN" ] || die "нет $BIN — сначала соберите проект:
  cd $REPO && mkdir -p build && cd build && cmake .. && make -j4"

if [ ! -x "$PROBE" ]; then
    say "== probe не собран — компилирую на месте"
    command -v g++ >/dev/null || die "нет ни $PROBE, ни g++"
    mkdir -p "$D"
    g++ -fPIC -O1 -o "$D/probe" "$SCRIPT_DIR/probe.cpp" \
        $(pkg-config --cflags --libs Qt5Widgets) || die "probe не собрался"
    PROBE="$D/probe"
fi

# ---------------------------------------------------------------- окружение
if ! xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
    command -v Xvfb >/dev/null || die "X-сервер $DISPLAY недоступен и нет Xvfb (sudo apt install xvfb)"
    Xvfb "$DISPLAY" -screen 0 1024x768x24 >/dev/null 2>&1 &
    XVFB_STARTED=1
    for i in $(seq 1 30); do
        xdpyinfo -display "$DISPLAY" >/dev/null 2>&1 && break
        sleep 0.3
    done
    xdpyinfo -display "$DISPLAY" >/dev/null 2>&1 || die "Xvfb не поднялся на $DISPLAY"
    say "== Поднят Xvfb на $DISPLAY"
    # Кириллица в ТЕКУЩЕЙ группе keymap: если символы есть только во второй
    # группе (us,ru при активном us), xdotool временно переназначает keycode,
    # и приложение может декодировать нажатия ещё старой раскладкой (гонка
    # MappingNotify — символы теряются/подменяются; с ru ПЕРВОЙ группой
    # замерено 8/8 чисто, с us первой — ~25% сбоев). Только для СВОЕГО Xvfb:
    # живую сессию не трогаем.
    if command -v setxkbmap >/dev/null; then
        setxkbmap -layout ru,us 2>/dev/null \
            && say "== Раскладка keymap: ru,us (кириллица в текущей группе — посимвольная печать детерминирована)" \
            || say "== setxkbmap не сработал — посимвольные сценарии чувствительны к гонке MappingNotify"
    else
        say "== Нет setxkbmap (x11-xkb-utils) — посимвольные сценарии чувствительны к гонке MappingNotify"
    fi
fi

# WM нужен EWMH-совместимый (windowactivate/getactivewindow). Проверяем не
# pgrep'ом (он ловит и умирающий процесс — стенд останется без WM), а по
# _NET_SUPPORTING_WM_CHECK на корневом окне: метку ставит только живой WM.
if ! xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q '#'; then
    command -v openbox >/dev/null || die "нет WM: sudo apt install openbox"
    openbox >/dev/null 2>&1 &
    OPENBOX_STARTED=1
    for i in $(seq 1 40); do
        xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q '#' && break
        sleep 0.2
    done
    xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q '#' \
        || die "openbox не зарегистрировался как WM на $DISPLAY"
    say "== Поднят openbox"
fi

rm -rf "$D"; mkdir -p "$D"

# ---------------------------------------------------------------- утилиты
start_probe() {  # $1=appName(WM_CLASS) $2=заголовок $3=дамп [$4=жизнь мс]
    "$PROBE" "$1" "$2" "$3" "${4:-180000}" >/dev/null 2>&1 &
    echo $!
}
wait_win() {  # $1=заголовок -> WID
    local i w=""
    for i in $(seq 1 60); do
        w=$(xdotool search --name "$1" 2>/dev/null | head -1)
        [ -n "$w" ] && { echo "$w"; return 0; }
        sleep 0.2
    done
    return 1
}
field() { grep "^$2=" "$1" 2>/dev/null | head -1 | cut -d= -f2-; }
mkconf() {  # $1=файл $2=method $3=pin_mode $4=pin_restore_focus
    cat > "$1" <<EOF
[output]
method=$2
pin_window=true
pin_mode=$3
pin_activate_ms=150
pin_restore_focus=$4
preserve_clipboard=false
typing_delay_ms=5
own_window_class=voice-assistant
EOF
}
kill_all_probes() {
    pkill -x probe 2>/dev/null
    # Дождаться, пока старые окна исчезнут из X: иначе wait_win следующего
    # сценария схватит WID умирающего окна с тем же заголовком, и первая
    # вставка уйдёт в призрак.
    local i
    for i in $(seq 1 30); do
        pgrep -x probe >/dev/null || {
            [ -z "$(xdotool search --name 'Editor' 2>/dev/null)" ] \
                && [ -z "$(xdotool search --name 'Tray Menu' 2>/dev/null)" ] && return 0
        }
        sleep 0.1
    done
    return 0
}

say "== Стенд: bin=$BIN"
say "==        probe=$PROBE"
say "==        display=$DISPLAY, данные в $D"

# ============================================================================
scen "activate + буфер обмена: текст в привязанное окно, фокус возвращается"
kill_all_probes
mkconf "$D/c1.ini" auto activate true
p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
p2=$(start_probe EditorB "Editor B" "$D/b.dump"); w2=$(wait_win "Editor B")
xdotool windowactivate --sync "$w1"
"$BIN" --type "привет мир один" --pin-active --delay 2000 --config "$D/c1.ini" \
    > "$D/s1.log" 2>&1 &
tp=$!; sleep 0.7
xdotool windowactivate --sync "$w2"          # «пользователь переключился»
wait $tp; rc1=$?
sleep 0.5
check "текст дошёл в привязанное A" "$(field "$D/a.dump" text)" "привет мир один"
check "B осталось пустым"           "$(field "$D/b.dump" text)" ""
check "фокус вернулся в B"          "$(xdotool getactivewindow)" "$w2"
check "код возврата 0"              "$rc1" "0"
has "$D/s1.log" "ВСТАВЛЕНО"
has "$D/s1.log" "активация + настоящие события"

# ============================================================================
scen "sendevent + буфер обмена: синтетика в расфокусированное окно, фокус не тронут"
kill_all_probes
mkconf "$D/c2.ini" auto sendevent true
p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
p2=$(start_probe EditorB "Editor B" "$D/b.dump"); w2=$(wait_win "Editor B")
xdotool windowactivate --sync "$w1"
"$BIN" --type "привет мир два" --pin-active --delay 1500 --config "$D/c2.ini" \
    > "$D/s2.log" 2>&1 &
tp=$!; sleep 0.7
xdotool windowactivate --sync "$w2"
wait $tp; rc2=$?
sleep 0.5
check "текст дошёл в A (XSendEvent)" "$(field "$D/a.dump" text)" "привет мир два"
check "события синтетические"        "$([ "$(field "$D/a.dump" synthetic)" -gt 0 ] && echo yes)" "yes"
check "B осталось пустым"            "$(field "$D/b.dump" text)" ""
check "фокус не переключался (B)"    "$(xdotool getactivewindow)" "$w2"
check "код возврата 0"               "$rc2" "0"
has "$D/s2.log" "синтетика (--window)"

# ============================================================================
scen "окно УЖЕ в фокусе: доставка БЕЗ --window, всегда настоящими событиями"
# Самый частый случай диктовки: стоим в редакторе, никуда не переключались.
# Прежняя ветка «--window в сфокусированное окно» на живом Cinnamon + XED
# (GTK3) молча не доходила (грабля №29): без --window xdotool всегда шлёт
# XTest — от WM и тулкита не зависит. Проверяем счётчиками пробника.
kill_all_probes
mkconf "$D/c3.ini" auto activate true
p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
xdotool windowactivate --sync "$w1"; sleep 0.4
"$BIN" --type "фокус на месте" --pin-active --config "$D/c3.ini" > "$D/s3.log" 2>&1
rc3=$?
sleep 0.5
check "текст дошёл"               "$(field "$D/a.dump" text)" "фокус на месте"
check "события НАСТОЯЩИЕ (XTest)" "$([ "$(field "$D/a.dump" synthetic)" -eq 0 ] && echo yes)" "yes"
check "нажатия посчитаны"         "$([ "$(field "$D/a.dump" real)" -gt 0 ] && echo yes)" "yes"
check "код возврата 0"            "$rc3" "0"
has "$D/s3.log" "активное окно"
check "фокус не ушёл"             "$(xdotool getactivewindow)" "$w1"
check "без ложной тревоги"        "$(grep -c 'недоступно' "$D/s3.log")" "0"

# ============================================================================
scen "activate + посимвольная печать (method=xdotool): кириллица без раскладки"
# Известная среда-гонка (README, «Известные особенности»): после переезда
# фокуса первые нажатия могут декодироваться приложением до полной обработки
# FocusIn, своё добавляет переназначение keycode. Поэтому здесь увеличены
# паузы и разрешено до 3 попыток; способ «буфер обмена» (дефолт) от этого
# не страдает вовсе. Систематическая поломка (3 неудачи подряд) — всё ещё
# однозначный FAIL.
kill_all_probes
mkconf "$D/c4.ini" xdotool activate true
sed -i 's/^typing_delay_ms=.*/typing_delay_ms=20/; s/^pin_activate_ms=.*/pin_activate_ms=300/' "$D/c4.ini"
s4ok=""
for attempt in 1 2 3; do
    kill_all_probes
    p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
    p2=$(start_probe EditorB "Editor B" "$D/b.dump"); w2=$(wait_win "Editor B")
    xdotool windowactivate --sync "$w1"
    sleep 0.5
    "$BIN" --type "тест три четыре" --pin-active --delay 1500 --config "$D/c4.ini" \
        > "$D/s4.log" 2>&1 &
    tp=$!; sleep 0.7
    xdotool windowactivate --sync "$w2"          # уходим — доставка только через активацию
    wait $tp
    sleep 0.5
    if [ "$(field "$D/a.dump" text)" = "тест три четыре" ]; then
        s4ok="pass"; say "    [..]   попытка $attempt: чисто"
        break
    fi
    say "    [..]   попытка $attempt: «$(field "$D/a.dump" text)» — гонка среды, повторяю"
done
check "кириллица дошла посимвольно (≤3 попыток)" "$s4ok" "pass"
check "события настоящие"           "$([ "$(field "$D/a.dump" real)" -gt 0 ] && echo yes)" "yes"
check "B пустое"                    "$(field "$D/b.dump" text)" ""
has "$D/s4.log" "теряются отдельные символы"   # разовое честное предупреждение
has "$D/s4.log" "активация + настоящие события"

# ============================================================================
scen "pin_restore_focus=false: фокус остаётся в целевом окне"
kill_all_probes
mkconf "$D/c5.ini" auto activate false
p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
p2=$(start_probe EditorB "Editor B" "$D/b.dump"); w2=$(wait_win "Editor B")
xdotool windowactivate --sync "$w1"
"$BIN" --type "фокус останется" --pin-active --delay 1500 --config "$D/c5.ini" \
    > "$D/s5.log" 2>&1 &
tp=$!; sleep 0.7
xdotool windowactivate --sync "$w2"
wait $tp
sleep 0.5
check "текст дошёл в A"        "$(field "$D/a.dump" text)" "фокус останется"
check "фокус НЕ возвращался"   "$(xdotool getactivewindow)" "$w1"

# ============================================================================
scen "закрытое окно: привязка снимается, текст идёт в активное окно"
kill_all_probes
mkconf "$D/c6.ini" auto activate true
p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
p2=$(start_probe EditorB "Editor B" "$D/b.dump"); w2=$(wait_win "Editor B")
xdotool windowactivate --sync "$w1"
"$BIN" --type "текст шесть" --pin-active --delay 2500 --config "$D/c6.ini" \
    > "$D/s6.log" 2>&1 &
tp=$!; sleep 0.8
kill -9 $p1                                   # «пользователь закрыл окно»
sleep 1.0
xdotool windowactivate --sync "$w2" 2>/dev/null
wait $tp; rc6=$?
sleep 0.5
check "текст ушёл в активное B" "$(field "$D/b.dump" text)" "текст шесть"
check "код возврата 0"          "$rc6" "0"
has "$D/s6.log" "недоступно"

# ============================================================================
scen "своё окно (WM_CLASS=voice-assistant): привязка не устанавливается"
kill_all_probes
mkconf "$D/c7.ini" auto activate true
p1=$(start_probe voice-assistant "Tray Menu" "$D/a.dump"); w1=$(wait_win "Tray Menu")
xdotool windowactivate --sync "$w1"; sleep 0.4
"$BIN" --pin-info --config "$D/c7.ini" > "$D/s7a.log" 2>&1; rc7a=$?
check "--pin-info вернул 1 (отказ)" "$rc7a" "1"
has "$D/s7a.log" "ОТКАЗ"
has "$D/s7a.log" "принадлежит самому помощнику"
"$BIN" --type "текст семь" --pin-active --delay 800 --config "$D/c7.ini" > "$D/s7.log" 2>&1
sleep 0.5
has "$D/s7.log" "принадлежит самому помощнику"

# ============================================================================
scen "--pin-info: диагностика без микрофона и GUI"
kill_all_probes
p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
xdotool windowactivate --sync "$w1"; sleep 0.4
"$BIN" --pin-info --config "$D/c1.ini" > "$D/s8.log" 2>&1; rc8=$?
check "код возврата 0" "$rc8" "0"
has "$D/s8.log" "Активное окно"
has "$D/s8.log" "pin_mode"
has "$D/s8.log" "привязка установлена"
has "$D/s8.log" "$w1"

# ============================================================================
scen "sendevent + буфер: две вставки подряд в расфокусированное окно"
kill_all_probes
mkconf "$D/c9.ini" auto sendevent true
p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
p2=$(start_probe EditorB "Editor B" "$D/b.dump"); w2=$(wait_win "Editor B")
xdotool windowactivate --sync "$w1"
"$BIN" --type "альфа " --pin-active --delay 1200 --config "$D/c9.ini" > "$D/s9.log" 2>&1 &
tp=$!; sleep 0.6; xdotool windowactivate --sync "$w2"; wait $tp
xdotool windowactivate --sync "$w1"
"$BIN" --type "бета" --pin-active --delay 1200 --config "$D/c9.ini" >> "$D/s9.log" 2>&1 &
tp=$!; sleep 0.6; xdotool windowactivate --sync "$w2"; wait $tp
sleep 0.5
check "обе вставки дошли в A"  "$(field "$D/a.dump" text)" "альфа бета"
check "B пустое"              "$(field "$D/b.dump" text)" ""
check "две строки лога"        "$(grep -c 'XdotoolInjector: текст' "$D/s9.log")" "2"

# ============================================================================
scen "две вставки подряд (activate): проверка живости окна на каждой вставке"
kill_all_probes
mkconf "$D/c10.ini" auto activate true
p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
xdotool windowactivate --sync "$w1"
sleep 0.5   # openbox: свежее окно может не получить фокус мгновенно (_NET_WM_DESKTOP)
"$BIN" --type "альфа " --pin-active --delay 800 --config "$D/c10.ini" > "$D/s10.log" 2>&1
sleep 0.5   # в живой диктовке сегменты VAD приходят не быстрее чем раз в 0.5 с
"$BIN" --type "бета"   --pin-active --delay 800 --config "$D/c10.ini" >> "$D/s10.log" 2>&1
sleep 0.5
check "обе вставки дошли" "$(field "$D/a.dump" text)" "альфа бета"
check "две строки лога"   "$(grep -c 'XdotoolInjector: текст' "$D/s10.log")" "2"

# ============================================================================
scen "--window WID: явная привязка без --pin-active"
kill_all_probes
mkconf "$D/c11.ini" auto sendevent true
p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
p2=$(start_probe EditorB "Editor B" "$D/b.dump"); w2=$(wait_win "Editor B")
xdotool windowactivate --sync "$w2"; sleep 0.4
"$BIN" --type "явная привязка" --window "$w1" --delay 800 --config "$D/c11.ini" \
    > "$D/s11.log" 2>&1 &
tp=$!; sleep 0.5; xdotool windowactivate --sync "$w1"; xdotool windowactivate --sync "$w2"; wait $tp
sleep 0.5
check "текст дошёл в явно указанное A" "$(field "$D/a.dump" text)" "явная привязка"
check "B пустое"                       "$(field "$D/b.dump" text)" ""

# ============================================================================
scen "без привязки: текст идёт в текущее активное окно"
kill_all_probes
mkconf "$D/c12.ini" auto activate true
p1=$(start_probe EditorA "Editor A" "$D/a.dump"); w1=$(wait_win "Editor A")
xdotool windowactivate --sync "$w1"; sleep 0.4
"$BIN" --type "просто двенадцать" --delay 500 --config "$D/c12.ini" > "$D/s12.log" 2>&1
sleep 0.5
check "текст в активном окне" "$(field "$D/a.dump" text)" "просто двенадцать"
has "$D/s12.log" "активное окно"

# ============================================================================
scen "ошибки CLI: пустой текст и кривой --delay"
"$BIN" --type > "$D/s13.log" 2>&1; rcA=$?
"$BIN" --type "текст" --delay abc --config "$D/c1.ini" > "$D/s13b.log" 2>&1; rcB=$?
check "--type без текста -> 2"     "$rcA" "2"
check "--delay abc -> 2"           "$rcB" "2"

# ---------------------------------------------------------------- итог
kill_all_probes
say ""
say "=============================================="
say "ИТОГ: сценариев $SCEN, проверок ok=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ] && say "ВСЁ ЗЕЛЁНОЕ" || say "ЕСТЬ ПАДЕНИЯ — см. логи в $D"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
