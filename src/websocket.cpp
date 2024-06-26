#include <bit>
#include <thread>
#include <vector>

#include <libwebsockets.h>

#include "config.hpp"
#include "macros/assert.hpp"
#include "util/assert.hpp"
#include "util/writers-reader-buffer.hpp"
#include "websocket.hpp"

namespace ws {
enum class ConnectionState {
    Initialized,
    Connected,
    Destroyed,
};

struct Connection {
    lws_context*                     context;
    lws*                             wsi;
    std::vector<std::byte>           receive_buffer;
    std::vector<Receiver>            receivers;
    ConnectionState                  conn_state;
    std::thread                      worker;
    WritersReaderBuffer<std::string> buffer_to_send;
};

namespace {
auto write_back(lws* const wsi, const void* const data, size_t size) -> int {
    auto buffer       = std::vector<std::byte>(LWS_SEND_BUFFER_PRE_PADDING + size + LWS_SEND_BUFFER_POST_PADDING);
    auto payload_head = buffer.data() + LWS_SEND_BUFFER_PRE_PADDING;
    memcpy(payload_head, data, size);
    return lws_write(wsi, std::bit_cast<unsigned char*>(payload_head), size, LWS_WRITE_TEXT);
}

auto write_back_str(lws* const wsi, const std::string_view str) -> int {
    return write_back(wsi, str.data(), str.size());
}

auto invoke_receiver(Connection* const conn, const std::span<std::byte> payload) -> void {
    auto& rxs = conn->receivers;
    for(auto i = rxs.begin(); i < rxs.end(); i += 1) {
        switch((*i)(payload)) {
        case ReceiverResult::Ignored:
            break;
        case ReceiverResult::Complete:
            rxs.erase(i);
            [[fallthrough]];
        case ReceiverResult::Handled:
            i = rxs.end();
            break;
        }
    }
}

auto append(std::vector<std::byte>& vec, void* in, const size_t len) -> void {
    const auto ptr = std::bit_cast<std::byte*>(in);
    vec.insert(vec.end(), ptr, ptr + len);
}

auto xmpp_callback(lws* wsi, lws_callback_reasons reason, void* const /*user*/, void* const in, const size_t len) -> int {
    const auto proto = lws_get_protocol(wsi);
    if(config::debug_websocket) {
        PRINT(__func__, " reason=", int(reason), " protocol=", proto);
    }
    if(proto == NULL) {
        return 0;
    }
    const auto conn = std::bit_cast<Connection*>(proto->user);
    if(conn->conn_state == ConnectionState::Destroyed) {
        return -1;
    }

    switch(reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        if(config::debug_websocket) {
            PRINT(__func__, " connection established");
        }
        conn->conn_state = ConnectionState::Connected;
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        if(config::debug_websocket) {
            PRINT(__func__, " connection error");
        }
        conn->conn_state = ConnectionState::Destroyed;
        break;
    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_CLIENT_CLOSED:
        if(config::debug_websocket) {
            PRINT(__func__, " connection close");
        }
        conn->conn_state = ConnectionState::Destroyed;
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        if(config::dump_websocket_packets) {
            auto str = std::string_view(std::bit_cast<char*>(in));
            PRINT(">>> ", str);
        }
        const auto remaining = lws_remaining_packet_payload(wsi);
        const auto final     = lws_is_final_fragment(wsi);
        if(remaining != 0 || !final) {
            append(conn->receive_buffer, in, len);
            break;
        }
        auto complete_payload = std::span<std::byte>();
        if(conn->receive_buffer.empty()) {
            complete_payload = std::span<std::byte>(std::bit_cast<std::byte*>(in), len);
        } else {
            append(conn->receive_buffer, in, len);
            complete_payload = conn->receive_buffer;
        }
        invoke_receiver(conn, complete_payload);
        conn->receive_buffer.clear();
    } break;
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if(config::debug_websocket) {
            PRINT(__func__, " writeable");
        }
        for(auto& buf : conn->buffer_to_send.swap()) {
            write_back_str(conn->wsi, buf);
        }
        break;
    default:
        if(config::debug_websocket) {
            PRINT(__func__, " unhandled callback");
        }
        break;
    }

    return 0;
}

auto connection_worker_main(Connection* const conn) -> void {
    while(conn->conn_state != ConnectionState::Destroyed) {
        lws_service(conn->context, 0);
    }
    lws_context_destroy(conn->context);
}

} // namespace

auto create_connection(const char* const address, const uint32_t port, const char* const path, const bool secure) -> Connection* {
    lws_set_log_level(config::libws_loglevel_bitmap, NULL);

    auto conn = std::unique_ptr<Connection>(new Connection{
        .context    = NULL,
        .wsi        = NULL,
        .conn_state = ConnectionState::Initialized,
    });

    const auto xmpp = lws_protocols{
        .name           = "xmpp",
        .callback       = xmpp_callback,
        .rx_buffer_size = 0x1000 * 1,
        .user           = conn.get(),
        .tx_packet_size = 0x1000 * 1,
    };
    const auto protocols = std::array{&xmpp, (const lws_protocols*)NULL};

    const auto context_creation_info = lws_context_creation_info{
        .port       = CONTEXT_PORT_NO_LISTEN,
        .gid        = gid_t(-1),
        .uid        = uid_t(-1),
        .options    = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT,
        .pprotocols = (const lws_protocols**)protocols.data(),
    };

    const auto context = lws_create_context(&context_creation_info);
    DYN_ASSERT(context != NULL);

    const auto host      = build_string(address, ":", port);
    const auto ssl_flags = secure ? LCCSCF_USE_SSL : LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;

    const auto client_connect_info = lws_client_connect_info{
        .context                   = context,
        .address                   = address,
        .port                      = int(port),
        .ssl_connection            = ssl_flags,
        .path                      = path,
        .host                      = host.data(),
        .protocol                  = xmpp.name,
        .ietf_version_or_minus_one = -1,
    };
    const auto wsi = lws_client_connect_via_info(&client_connect_info);
    DYN_ASSERT(wsi != NULL);

    while(conn->conn_state == ConnectionState::Initialized) {
        lws_service(context, 50);
    }

    conn->context = context;
    conn->wsi     = wsi;
    conn->worker  = std::thread(connection_worker_main, conn.get());

    return conn.release();
}

auto free_connection(Connection* const conn) -> void {
    conn->conn_state = ConnectionState::Destroyed;
    lws_callback_on_writable(conn->wsi);
    conn->worker.join();
    delete conn;
}

auto send_str(Connection* const conn, const std::string_view str) -> void {
    if(config::dump_websocket_packets) {
        PRINT("<<< ", str);
    }
    conn->buffer_to_send.push(std::string(str));
    lws_callback_on_writable(conn->wsi);
}

auto add_receiver(Connection* conn, const Receiver receiver) -> void {
    conn->receivers.push_back(receiver);
}
} // namespace ws
