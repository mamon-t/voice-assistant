# tools/e2e — стенд сквозных проверок привязки вывода к окну (pin_window).
#
# probe: тестовое Qt-окно с QLineEdit и перехватом сырых xcb-событий —
# видно, настоящими событиями пришёл ввод или синтетикой XSendEvent.
# Сам стенд — run-e2e.sh, ему нужны Xvfb, openbox, xdotool и xclip
# (в ctest НЕ регистрируется: требует X-окружения, запускается вручную).

add_executable(probe probe.cpp)
target_link_libraries(probe PRIVATE Qt5::Widgets)
