#include "../macros/logger.hpp"
#include "../util/charconv.hpp"
#include "../xml/xml.hpp"
#include "common.hpp"
#include "jingle.hpp"

#define CUTIL_MACROS_PRINT_FUNC(...) LOG_ERROR(logger, __VA_ARGS__)
#include "../macros/unwrap.hpp"

namespace jingle {
namespace {
auto logger = Logger("xmpp");

template <class T>
auto parse_template(const xml::Node& node) -> std::optional<T> {
    auto r     = T{};
    auto found = false;

    for(const auto& a : node.attrs) {
        if(0) {
            found = true;
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found, "required attributes not found");
    for(const auto& c : node.children) {
        if(0) {
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}

#define num_or_nullopt(field, value)                     \
    {                                                    \
        unwrap(num, from_chars<decltype(field)>(value)); \
        field = num;                                     \
    }

#define unwrap_parsed_or_nullopt(opt, field) \
    {                                        \
        unwrap_mut(value, opt);              \
        field.push_back(std::move(value));   \
    }

#define search_str_array_or_null(arr, field, value)             \
    {                                                           \
        unwrap(num, arr.find(value), "unknown enum {}", value); \
        field = num;                                            \
    }

template <bool optional_value>
auto parse_parameter(const xml::Node& node, const std::string_view ns) -> std::optional<Parameter<optional_value>> {
    auto r           = Parameter<optional_value>{};
    auto found_name  = false;
    auto found_value = false;

    for(const auto& a : node.attrs) {
        if(a.key == "name") {
            r.name     = a.value;
            found_name = true;
        } else if(a.key == "value") {
            r.value     = a.value;
            found_value = true;
        } else if(a.key == "xmlns") {
            ensure(a.value == ns, "unsupported xmlns {}", a.value);
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_name && (optional_value || found_value), "required attributes not found");
    for(const auto& c : node.children) {
        if(0) {
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}

auto parse_rtcp_fb(const xml::Node& node) -> std::optional<Jingle::Content::RTPDescription::PayloadType::RTCPFeedBack> {
    auto r          = Jingle::Content::RTPDescription::PayloadType::RTCPFeedBack{};
    auto found_type = false;

    for(const auto& a : node.attrs) {
        if(a.key == "type") {
            found_type = true;
            r.type     = a.value;
        } else if(a.key == "subtype") {
            r.subtype = a.value;
        } else if(a.key == "xmlns") {
            ensure(a.value == ns::rtp_rtcp_fb, "unsupported xmlns {}", a.value);
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_type, "required attributes not found");
    for(const auto& c : node.children) {
        if(0) {
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}

auto parse_payload_type(const xml::Node& node) -> std::optional<Jingle::Content::RTPDescription::PayloadType> {
    auto r        = Jingle::Content::RTPDescription::PayloadType{};
    auto found_id = 0;

    for(const auto& a : node.attrs) {
        if(a.key == "id") {
            num_or_nullopt(r.id, a.value);
            found_id = true;
        } else if(a.key == "clockrate") {
            num_or_nullopt(r.clockrate, a.value);
        } else if(a.key == "channels") {
            num_or_nullopt(r.channels, a.value);
        } else if(a.key == "name") {
            r.name = a.value;
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_id, "required attributes not found");
    for(const auto& c : node.children) {
        if(c.name == "rtcp-fb") {
            unwrap_parsed_or_nullopt(parse_rtcp_fb(c), r.rtcp_fbs);
        } else if(c.name == "parameter") {
            unwrap_parsed_or_nullopt(parse_parameter<false>(c, {}), r.parameters);
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}

auto parse_source(const xml::Node& node) -> std::optional<Jingle::Content::RTPDescription::Source> {
    auto r          = Jingle::Content::RTPDescription::Source{};
    auto found_ssrc = false;

    for(const auto& a : node.attrs) {
        if(a.key == "ssrc") {
            num_or_nullopt(r.ssrc, a.value);
            found_ssrc = true;
        } else if(a.key == "xmlns") {
            ensure(a.value == ns::rtp_ssma, "unsupported xmlns {}", a.value);
        } else if(a.key == "name") {
            r.name = a.value;
        } else if(a.key == "videoType") {
            r.video_type = a.value;
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_ssrc, "required attributes not found");
    auto found_owner = false;
    for(const auto& c : node.children) {
        if(c.name == "parameter") {
            unwrap_parsed_or_nullopt(parse_parameter<true>(c, ns::rtp), r.parameters);
        } else if(c.name == "ssrc-info") {
            ensure(c.is_attr_equal("xmlns", ns::jitsi_jitmeet), "invalid ssrc-info");
            if(const auto owner_o = c.find_attr("owner"); owner_o) {
                r.owner     = *owner_o;
                found_owner = true;
            } else {
                bail("ssrc-info has no owner");
            }
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    ensure(found_owner, "required children not found");
    return r;
}

auto parse_rtp_header_ext(const xml::Node& node) -> std::optional<Jingle::Content::RTPDescription::RTPHeaderExt> {
    auto r         = Jingle::Content::RTPDescription::RTPHeaderExt{};
    auto found_id  = false;
    auto found_uri = false;

    for(const auto& a : node.attrs) {
        if(a.key == "id") {
            num_or_nullopt(r.id, a.value);
            found_id = true;
        } else if(a.key == "uri") {
            r.uri     = a.value;
            found_uri = true;
        } else if(a.key == "xmlns") {
            ensure(a.value == ns::rtp_headerext, "unsupported xmlns {}", a.value);
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_id && found_uri, "required attributes not found");
    for(const auto& c : node.children) {
        if(0) {
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}

auto parse_ssrc_group(const xml::Node& node) -> std::optional<Jingle::Content::RTPDescription::SSRCGroup> {
    auto r               = Jingle::Content::RTPDescription::SSRCGroup{};
    auto found_semantics = false;

    for(const auto& a : node.attrs) {
        if(a.key == "semantics") {
            search_str_array_or_null(ssrc_group_semantics_str, r.semantics, a.value);
            found_semantics = true;
        } else if(a.key == "xmlns") {
            ensure(a.value == ns::rtp_ssma, "unsupported xmlns {}", a.value);
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_semantics, "required attributes not found");
    for(const auto& c : node.children) {
        if(c.name == "source") {
            unwrap(attr, c.find_attr("ssrc"), "source has not ssrc attribute");
            auto ssrc = uint32_t(0);
            num_or_nullopt(ssrc, attr);
            r.ssrcs.push_back(ssrc);
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}

auto parse_rtp_description(const xml::Node& node) -> std::optional<Jingle::Content::RTPDescription> {
    auto r = Jingle::Content::RTPDescription{};

    for(const auto& a : node.attrs) {
        if(a.key == "media") {
            r.media = a.value;
        } else if(a.key == "xmlns") {
            ensure(a.value == ns::rtp, "unsupported xmlns {}", a.value);
        } else if(a.key == "ssrc") {
            num_or_nullopt(r.ssrc, a.value);
        } else if(a.key == "maxptime") {
            // TODO: handle maxptime
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    for(const auto& c : node.children) {
        if(c.name == "payload-type") {
            unwrap_parsed_or_nullopt(parse_payload_type(c), r.payload_types);
        } else if(c.name == "source") {
            unwrap_parsed_or_nullopt(parse_source(c), r.sources);
        } else if(c.name == "rtp-hdrext") {
            unwrap_parsed_or_nullopt(parse_rtp_header_ext(c), r.rtp_header_exts);
        } else if(c.name == "ssrc-group") {
            unwrap_parsed_or_nullopt(parse_ssrc_group(c), r.ssrc_groups);
        } else if(c.name == "rtcp-mux") {
            r.support_mux = true;
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}

auto parse_fingerprint(const xml::Node& node) -> std::optional<Jingle::Content::IceUdpTransport::FingerPrint> {
    ensure(!node.data.empty(), "empty fingerprint");

    auto r = Jingle::Content::IceUdpTransport::FingerPrint{
        .hash = std::string(node.data),
    };
    auto found_hash  = false;
    auto found_setup = false;

    for(const auto& a : node.attrs) {
        if(a.key == "hash") {
            r.hash_type = a.value;
            found_hash  = true;
        } else if(a.key == "setup") {
            r.setup     = a.value;
            found_setup = true;
        } else if(a.key == "required") {
            if(a.value == "true") {
                r.required = true;
            } else if(a.value == "false") {
                r.required = false;
            } else {
                bail("invalid required");
            }
        } else if(a.key == "xmlns") {
            ensure(a.value == ns::dtls, "unsupported xmlns {}", a.value);
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_hash && found_setup, "required attributes not found");
    for(const auto& c : node.children) {
        if(0) {
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}

auto parse_candidate(const xml::Node& node) -> std::optional<Jingle::Content::IceUdpTransport::Candidate> {
    auto r                = Jingle::Content::IceUdpTransport::Candidate{};
    auto found_component  = false;
    auto found_generation = false;
    auto found_port       = false;
    auto found_priority   = false;
    auto found_type       = false;
    auto found_foundation = false;
    auto found_id         = false;
    auto found_ip_addr    = false;

    for(const auto& a : node.attrs) {
        if(a.key == "component") {
            num_or_nullopt(r.component, a.value);
            found_component = true;
        } else if(a.key == "generation") {
            num_or_nullopt(r.generation, a.value);
            found_generation = true;
        } else if(a.key == "port") {
            num_or_nullopt(r.port, a.value);
            found_port = true;
        } else if(a.key == "priority") {
            num_or_nullopt(r.priority, a.value);
            found_priority = true;
        } else if(a.key == "type") {
            search_str_array_or_null(candidate_type_str, r.type, a.value);
            found_type = true;
        } else if(a.key == "foundation") {
            r.foundation     = a.value;
            found_foundation = true;
        } else if(a.key == "id") {
            r.id     = a.value;
            found_id = true;
        } else if(a.key == "ip") {
            r.ip_addr     = a.value;
            found_ip_addr = true;
        } else if(a.key == "protocol") {
            ensure(a.value == "udp", "unsupported protocol {}", a.value);
        } else if(a.key == "network") {
            // ignore
        } else if(a.key == "rel-addr") {
            // ignore
        } else if(a.key == "rel-port") {
            // ignore
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_component && found_generation && found_port && found_priority && found_type && found_foundation && found_id && found_ip_addr, "required attributes not found");
    for(const auto& c : node.children) {
        if(0) {
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}

auto parse_ice_udp_transport(const xml::Node& node) -> std::optional<Jingle::Content::IceUdpTransport> {
    auto r           = Jingle::Content::IceUdpTransport{};
    auto found_pwd   = false;
    auto found_ufrag = false;

    for(const auto& a : node.attrs) {
        if(a.key == "pwd") {
            r.pwd     = a.value;
            found_pwd = true;
        } else if(a.key == "ufrag") {
            r.ufrag     = a.value;
            found_ufrag = true;
        } else if(a.key == "xmlns") {
            ensure(a.value == ns::transport_ice_udp, "unsupported xmlns {}", a.value);
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_pwd && found_ufrag, "required attributes not found");
    auto found_websocket = false;
    for(const auto& c : node.children) {
        if(c.name == "web-socket") {
            if(!c.is_attr_equal("xmlns", ns::jitsi_colibri)) {
                continue;
            }
            if(const auto url = c.find_attr("url"); url) {
                r.websocket     = *url;
                found_websocket = true;
            }
        } else if(c.name == "rtcp-mux") {
            r.support_mux = true;
        } else if(c.name == "fingerprint") {
            unwrap_parsed_or_nullopt(parse_fingerprint(c), r.fingerprints);
        } else if(c.name == "candidate") {
            unwrap_parsed_or_nullopt(parse_candidate(c), r.candidates);
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    ensure(found_websocket, "required children not found");
    return r;
}

auto parse_content(const xml::Node& node) -> std::optional<Jingle::Content> {
    auto r          = Jingle::Content{};
    auto found_name = false;

    for(const auto& a : node.attrs) {
        if(a.key == "name") {
            r.name     = a.value;
            found_name = true;
        } else if(a.key == "senders") {
            search_str_array_or_null(content_senders_str, r.senders, a.value);
        } else if(a.key == "creator") {
            if(a.value == "initiator") {
                r.is_from_initiator = true;
            } else if(a.value == "responder") {
                r.is_from_initiator = false;
            } else {
                bail("unknown creator {}", a.value);
            }
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_name, "required attributes not found");
    for(const auto& c : node.children) {
        if(c.name == "description") {
            if(const auto xmlns = c.find_attr("xmlns"); xmlns) {
                if(*xmlns == ns::rtp) {
                    unwrap_parsed_or_nullopt(parse_rtp_description(c), r.descriptions);
                } else {
                    LOG_WARN(logger, "unknown cowntent type {}", *xmlns);
                }
            } else {
                LOG_WARN(logger, "no xmlns {}", *xmlns);
            }
        } else if(c.name == "transport") {
            if(const auto xmlns = c.find_attr("xmlns"); !xmlns) {
                LOG_WARN(logger, "no xmlns in transport");
            } else if(*xmlns != ns::transport_ice_udp) {
                LOG_WARN(logger, "unsupported transport{}", *xmlns);
            } else {
                if(auto opt = parse_ice_udp_transport(c)) {
                    r.transports.push_back(std::move(*opt));
                }
            }
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}

auto parse_group(const xml::Node& node) -> std::optional<Jingle::Group> {
    auto r               = Jingle::Group{};
    auto found_semantics = false;

    for(const auto& a : node.attrs) {
        if(a.key == "semantics") {
            search_str_array_or_null(group_semantics_str, r.semantics, a.value);
            found_semantics = true;
        } else if(a.key == "xmlns") {
            ensure(a.value == ns::grouping, "unsupported xmlns {}", a.value);
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_semantics, "required attributes not found");
    for(const auto& c : node.children) {
        if(c.name == "content") {
            if(const auto name = c.find_attr("name"); name) {
                r.contents.emplace_back(*name);
            }
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}
} // namespace

auto parse(const xml::Node& node) -> std::optional<Jingle> {
    auto r            = Jingle{};
    auto found_action = false;
    auto found_sid    = false;

    for(const auto& a : node.attrs) {
        if(a.key == "action") {
            search_str_array_or_null(action_str, r.action, a.value);
            found_action = true;
        } else if(a.key == "sid") {
            r.sid     = a.value;
            found_sid = true;
        } else if(a.key == "initiator") {
            r.initiator = a.value;
        } else if(a.key == "responder") {
            r.responder = a.value;
        } else if(a.key == "xmlns") {
            ensure(a.value == ns::jingle, "unsupported xmlns {}", a.value);
        } else {
            LOG_WARN(logger, "unhandled attribute {}", a.key);
        }
    }
    ensure(found_action && found_sid, "required attributes not found");
    for(const auto& c : node.children) {
        if(c.name == "content") {
            unwrap_parsed_or_nullopt(parse_content(c), r.contents);
        } else if(c.name == "group") {
            unwrap(group, parse_group(c));
            r.group.reset(new Jingle::Group{group});
        } else if(c.name == "bridge-session") {
            // ignore
        } else {
            LOG_WARN(logger, "unhandled child {}", c.name);
        }
    }
    return r;
}
} // namespace jingle
