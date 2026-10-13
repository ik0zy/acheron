// The voice client's video in a build without cameras and screen shares, in the place of
// VoiceClientVideo.cpp. Such a build declares no video and offers to decode none, so the server
// sends none, and nothing here has anything to do.

#include "VoiceClient.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

void VoiceClient::setVideoFrameHandler(VideoFrameHandler) {}
void VoiceClient::setVideoWants(int, bool) {}
void VoiceClient::setVideoPlan(const MediaSinkWants &, const QSet<quint32> &) {}
void VoiceClient::requestVideoKeyframeFrom(Core::Snowflake) {}
void VoiceClient::requestVideoKeyframeFromEveryone() {}
void VoiceClient::withdrawVideoCodec(VideoCodec) {}

void VoiceClient::offerVideoEncoding(const QList<VideoCodec> &) {}
VideoCodec VoiceClient::videoSendCodec() const { return VideoCodec::Unknown; }
void VoiceClient::startVideo(const LocalVideoDescription &) {}
void VoiceClient::describeVideo(const LocalVideoDescription &) {}
void VoiceClient::setVideoBitrate(int) {}
void VoiceClient::stopVideo() {}
void VoiceClient::sendVideoFrame(const EncodedVideoFrame &) {}

void VoiceClient::onSessionUpdated(const SessionUpdate &) {}
void VoiceClient::onMediaSinkWants(const MediaSinkWants &) {}

void VoiceClient::cleanupVideo() {}
void VoiceClient::readVideoPayloadTypes() {}
void VoiceClient::noteTransportSequence(std::span<const uint8_t>) {}
void VoiceClient::receiveVideo(const ParsedRtp &, PacketPool::Handle, VideoCodec, bool) {}
void VoiceClient::receiveVideoFeedback(const ReceivedRtcp &) {}
void VoiceClient::receiveVideoAfresh(quint32) {}
void VoiceClient::applyRemoteVideo(const ClientConnectData &) {}
void VoiceClient::removeRemoteVideo(Core::Snowflake) {}
void VoiceClient::takeLocalVideoStreams(const VoiceReady &) {}
void VoiceClient::setVideoSendCodec(const QString &) {}
void VoiceClient::startVideoTimer() {}
void VoiceClient::sendVideoWants() {}
void VoiceClient::registerLocalVideoWithDave() {}
void VoiceClient::askForLocalKeyframe() {}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
