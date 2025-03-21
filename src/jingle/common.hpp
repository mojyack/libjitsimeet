#pragma once
#include "../util/pair-table.hpp"
#include "jingle.hpp"

namespace jingle {
namespace {
namespace ns {
constexpr auto jingle            = "urn:xmpp:jingle:1";
constexpr auto rtp               = "urn:xmpp:jingle:apps:rtp:1";
constexpr auto rtp_ssma          = "urn:xmpp:jingle:apps:rtp:ssma:0";
constexpr auto rtp_headerext     = "urn:xmpp:jingle:apps:rtp:rtp-hdrext:0";
constexpr auto rtp_rtcp_fb       = "urn:xmpp:jingle:apps:rtp:rtcp-fb:0";
constexpr auto dtls              = "urn:xmpp:jingle:apps:dtls:0";
constexpr auto grouping          = "urn:xmpp:jingle:apps:grouping:0";
constexpr auto transport_ice_udp = "urn:xmpp:jingle:transports:ice-udp:1";
constexpr auto jitsi_jitmeet     = "http://jitsi.org/jitmeet";
constexpr auto jitsi_colibri     = "http://jitsi.org/protocol/colibri";
} // namespace ns

inline const auto action_str = make_pair_table<Jingle::Action, std::string_view>({
    {Jingle::Action::ContentAccept, "content-accept"},
    {Jingle::Action::ContentAdd, "content-add"},
    {Jingle::Action::ContentModify, "content-modify"},
    {Jingle::Action::ContentReject, "content-reject"},
    {Jingle::Action::ContentRemove, "content-remove"},
    {Jingle::Action::DescriptionInfo, "description-info"},
    {Jingle::Action::SecurityInfo, "security-info"},
    {Jingle::Action::SessionAccept, "session-accept"},
    {Jingle::Action::SessionInfo, "session-info"},
    {Jingle::Action::SessionInitiate, "session-initiate"},
    {Jingle::Action::SessionTerminate, "session-terminate"},
    {Jingle::Action::TransportAccept, "transport-accept"},
    {Jingle::Action::TransportInfo, "transport-info"},
    {Jingle::Action::TransportReject, "transport-reject"},
    {Jingle::Action::TransportReplace, "transport-replace"},
    {Jingle::Action::SourceAdd, "source-add"},
    {Jingle::Action::SourceRemove, "source-remove"},
});

inline const auto ssrc_group_semantics_str = make_pair_table<Jingle::Content::RTPDescription::SSRCGroup::Semantics, std::string_view>({
    {Jingle::Content::RTPDescription::SSRCGroup::Semantics::Ls, "LS"},
    {Jingle::Content::RTPDescription::SSRCGroup::Semantics::Fid, "FID"},
    {Jingle::Content::RTPDescription::SSRCGroup::Semantics::Srf, "SRF"},
    {Jingle::Content::RTPDescription::SSRCGroup::Semantics::Anat, "ANAT"},
    {Jingle::Content::RTPDescription::SSRCGroup::Semantics::Fec, "FEC"},
    {Jingle::Content::RTPDescription::SSRCGroup::Semantics::Ddp, "DDP"},
});

inline const auto group_semantics_str = make_pair_table<Jingle::Group::Semantics, std::string_view>({
    {Jingle::Group::Semantics::LipSync, "LS"},
    {Jingle::Group::Semantics::Bundle, "BUNDLE"},
});

inline const auto candidate_type_str = make_pair_table<Jingle::Content::IceUdpTransport::Candidate::Type, std::string_view>({
    {Jingle::Content::IceUdpTransport::Candidate::Type::Host, "host"},
    {Jingle::Content::IceUdpTransport::Candidate::Type::Prflx, "prflx"},
    {Jingle::Content::IceUdpTransport::Candidate::Type::Relay, "relay"},
    {Jingle::Content::IceUdpTransport::Candidate::Type::Srflx, "srflx"},
});

inline const auto content_senders_str = make_pair_table<Jingle::Content::Senders, std::string_view>({
    {Jingle::Content::Senders::Both, "both"},
    {Jingle::Content::Senders::Initiator, "initiator"},
    {Jingle::Content::Senders::Responder, "responder"},
    {Jingle::Content::Senders::None, "none"},
});
} // namespace
} // namespace jingle
