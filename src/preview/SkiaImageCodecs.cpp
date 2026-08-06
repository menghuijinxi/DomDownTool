#include "preview/SkiaImageCodecs.h"

#include <mutex>

#include "include/codec/SkCodec.h"
#include "include/codec/SkJpegDecoder.h"
#include "include/codec/SkPngDecoder.h"
#include "include/codec/SkWebpDecoder.h"

void DomDownToolPreview::registerSkiaImageCodecs()
{
    static std::once_flag registrationFlag;
    std::call_once(registrationFlag, []
                   {
                       SkCodecs::Register(SkJpegDecoder::Decoder());
                       SkCodecs::Register(SkPngDecoder::Decoder());
                       SkCodecs::Register(SkWebpDecoder::Decoder());
                   });
}
