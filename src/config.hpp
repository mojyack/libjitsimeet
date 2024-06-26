#pragma once

namespace config {
inline auto libws_loglevel_bitmap  = 0b0111; // LLL_ERR | LLL_WARN | LLL_NOTICE
inline auto dump_websocket_packets = true;
inline auto debug_websocket        = true;
inline auto debug_xmpp_connection  = true;
inline auto debug_conference       = true;
inline auto debug_jingle_handler   = true;
inline auto debug_ice              = true;
inline auto debug_colibri          = true;
} // namespace config
