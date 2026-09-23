#include "AppDelegate.cpp"
#include "EngineLobbyScene.cpp"
#include "main.cpp"

static_assert(engine_lobby::packet::ping_req[0] == 'P');
static_assert(engine_lobby::packet::chat_notify[0] == 'C');
