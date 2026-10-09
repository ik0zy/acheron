#pragma once

#include <QByteArray>

#include <cstdint>

#include "VoiceEnums.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

class PacketBuffer;

// Discord's transport cipher for the rtpsize modes. RTP and RTCP share the key and the nonce
// counter and differ only in how many leading bytes stay readable (see RtpPacket.hpp).
class VoiceEncryption
{
public:
    VoiceEncryption(EncryptionMode mode, const QByteArray &secretKey);

    // The authentication tag and the 4-byte nonce appended to every packet.
    static constexpr int TRAILER_SIZE = 16 + 4;

    // Encrypts everything after the first clearSize bytes in place, authenticating those bytes,
    // and appends the trailer. The packet needs TRAILER_SIZE bytes of tailroom.
    bool seal(PacketBuffer &packet, int clearSize);

    // Reverses seal, leaving the clear bytes followed by the plaintext.
    bool open(PacketBuffer &packet, int clearSize);

    // Safe to call repeatedly; must have succeeded before any other use.
    static bool initialize();
    static bool isModeAvailable(EncryptionMode mode);

private:
    EncryptionMode m_mode;
    QByteArray m_key;
    uint32_t m_nonce = 0;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
