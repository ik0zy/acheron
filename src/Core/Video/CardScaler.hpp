#pragma once

#include <QRect>
#include <QSize>

#include <memory>

struct AVFrame;

namespace Acheron {
namespace Core {
namespace Video {

// Scales pictures that are on the graphics card and converts them to what an encoder takes,
// without taking them off it: Direct3D 11's video processor draws a part of one texture into
// another. ffmpeg's scale_d3d11 is the same processor and is not used. It asks for the pictures
// it makes as ten render targets in one texture array, which a Radeon RX 7800 XT for one will
// not make of NV12; it cannot cut a part out of a picture; and it leaves the colours of what it
// makes to the driver. Where the card is reached through VAAPI it is ffmpeg's scale_vaapi, which
// has none of those faults.
//
// Not thread safe; it belongs to the thread that feeds it.
class CardScaler
{
public:
    struct Output
    {
        QSize size;
        // The coefficients NV12 pictures are made with. They are in the video range.
        bool bt601 = false;
        // Where the card makes no NV12 pictures, BGRA ones will do: the encoder converts those.
        bool rgbWillDo = false;

        bool operator==(const Output &) const = default;
    };

    CardScaler();
    ~CardScaler();

    CardScaler(const CardScaler &) = delete;
    CardScaler &operator=(const CardScaler &) = delete;

    // False once the card has turned out not to do this, which the first picture finds out and
    // which is remembered for as long as the program runs.
    [[nodiscard]] static bool usable();

    // `area` of `from`, a BGRA picture on a graphics card whose opacity is left out of account,
    // drawn over `placed` of a new picture on the same card that is black everywhere else. The
    // new picture says which colours it has; a BGRA one is marked with those the encoder is to
    // convert it to, BT.709 in the video range. Pictures made one after the other for the same
    // output on the same card are of one kind to an encoder, whatever they were made from.
    // False when the card would not do it. Through VAAPI `placed` has to be the whole of the new
    // picture, and pictures are of one kind while those they are made from are of one size.
    bool scale(const AVFrame *from, const QRect &area, const Output &output, const QRect &placed, AVFrame *into);
    // The whole of `from`, or what its own cropping leaves of it, made the whole of the new picture.
    bool scale(const AVFrame *from, const Output &output, AVFrame *into);

private:
    struct Card;
    std::unique_ptr<Card> card;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
