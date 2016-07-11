/*
* Copyright (c) 2012-2016 Fredrik Mellbin
*
* This file is part of VapourSynth.
*
* VapourSynth is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* VapourSynth is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with VapourSynth; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

// This code is based on Avisynth's AVISource but most of it has
// been rewritten during the porting

#include <stdexcept>
#include "stdafx.h"

#include "VapourSynth.h"
#include "VSHelper.h"
#include "AVIReadHandler.h"
#include "../../common/p2p_api.h"
#include "../../common/fourcc.h"

static int ImageSize(const VSVideoInfo *vi, DWORD fourcc, int bitcount = 0) {
    int image_size;

    switch (fourcc) {
    case VS_FCC('v210'):
        image_size = ((16*((vi->width + 5) / 6) + 127) & ~127);
        image_size *= vi->height;
        break;
        // general packed
    case BI_RGB:
        image_size = BMPSizeHelper(vi->height, vi->width * bitcount / 8);
        break;
    case VS_FCC('b48r'):
        image_size = BMPSizeHelper(vi->height, vi->width * vi->format->bytesPerSample * 3);
        break;
    case VS_FCC('b64a'):
        image_size = BMPSizeHelper(vi->height, vi->width * vi->format->bytesPerSample * 4);
        break;
    case VS_FCC('YUY2'):
        image_size = BMPSizeHelper(vi->height, vi->width * 2);
        break;
    case VS_FCC('GREY'):
    case VS_FCC('Y800'):
    case VS_FCC('Y8  '):
        image_size = BMPSizeHelper(vi->height, vi->width * vi->format->bytesPerSample);
        break;
        // general planar
    default:
        image_size = (vi->width * vi->format->bytesPerSample) >> vi->format->subSamplingW;
        if (image_size) {
            image_size  *= vi->height;
            image_size >>= vi->format->subSamplingH;
            image_size  *= 2;
        }
        image_size += vi->width * vi->format->bytesPerSample * vi->height;
        image_size = (image_size + 3) & ~3;
    }
    return image_size;
}

static void unpackframe(const VSVideoInfo *vi, VSFrameRef *dst, VSFrameRef *dst_alpha, const uint8_t *srcp, int src_size, DWORD fourcc, int bitcount, bool flip, const VSAPI *vsapi) {
    bool padrows = false;

    const VSFormat *fi = vsapi->getFrameFormat(dst);
    p2p_buffer_param p = {};
    p.width = vsapi->getFrameWidth(dst, 0);
    p.height = vsapi->getFrameHeight(dst, 0);
    p.src[0] = srcp;
    p.src_stride[0] = vsapi->getFrameWidth(dst, 0) * 4 * fi->bytesPerSample;
    p.src[1] = (uint8_t *)p.src[0] + p.src_stride[0] * p.height;
    p.src_stride[1] = vsapi->getFrameWidth(dst, 0) * 4 * fi->bytesPerSample;
    for (int plane = 0; plane < fi->numPlanes; plane++) {
        p.dst[plane] = vsapi->getWritePtr(dst, plane);
        p.dst_stride[plane] = vsapi->getStride(dst, plane);
    }

    switch (fourcc) {
    case VS_FCC('P010'): p.packing = p2p_p010_le; p2p_unpack_frame(&p, 0); break;
    case VS_FCC('P210'): p.packing = p2p_p210_le; p2p_unpack_frame(&p, 0); break;
    case VS_FCC('P016'): p.packing = p2p_p016_le; p2p_unpack_frame(&p, 0); break;
    case VS_FCC('P216'): p.packing = p2p_p216_le; p2p_unpack_frame(&p, 0); break;
    case VS_FCC('Y416'): p.packing = p2p_y416_le; p2p_unpack_frame(&p, 0); break;
    case VS_FCC('v210'):
        p.packing = p2p_v210_le;
        p.src_stride[0] = ((16 * ((vi->width + 5) / 6) + 127) & ~127) / 4;
        p2p_unpack_frame(&p, 0);
        break;
    case BI_RGB:
        if (bitcount == 24)
            p.packing = p2p_rgb24_le;
        else if (bitcount == 32)
            p.packing = p2p_argb32_le;
        p.src_stride[0] = (vi->width*(bitcount/8) + 3) & ~3;
        if (flip) {
            p.src[0] = srcp + p.src_stride[0] * (p.height - 1);
            p.src_stride[0] = -p.src_stride[0];
        }
        p2p_unpack_frame(&p, 0);
        break;
    case VS_FCC('b48r'):
    case VS_FCC('b64a'):
        if (fourcc == VS_FCC('b48r'))
            p.packing = p2p_rgb48_be;
        else if (fourcc == VS_FCC('b64a'))
            p.packing = p2p_argb64_be;
        p.src_stride[0] = ((vi->width*vi->format->bytesPerSample*3 + 3) & ~3);
        if (flip) {
            p.src[0] = srcp + p.src_stride[0] * (p.height - 1);
            p.src_stride[0] = -p.src_stride[0];
        }
        p2p_unpack_frame(&p, 0);
        break;
    case VS_FCC('YUY2'):
        p.packing = p2p_yuy2;
        p.src_stride[0] = (vi->width*2+ 3) & ~3;
        p2p_unpack_frame(&p, 0);
        break;
    case VS_FCC('GREY'):
    case VS_FCC('Y800'):
    case VS_FCC('Y8  '):
        padrows = true;
        // general planar
    default:
        if (!padrows && src_size) {
            int packed_size = vi->height * vi->width * vi->format->bytesPerSample;
            if (vi->format->numPlanes == 3)
                packed_size += 2*(packed_size >> (vi->format->subSamplingH + vi->format->subSamplingW));
            if (((src_size + 3) & ~3) != ((packed_size + 3) & ~3))
                padrows = true;
        }
        for (int i = 0; i < vi->format->numPlanes; i++) {
            bool switchuv =  (fourcc != VS_FCC('I420') && fourcc != VS_FCC('Y41B'));
            int plane = i;
            if (switchuv) {
                if (i == 1)
                    plane = 2;
                else if (i == 2)
                    plane = 1;
            }

            int rowsize = vsapi->getFrameWidth(dst, plane) * vi->format->bytesPerSample;
            if (padrows)
                rowsize = (rowsize + 3) & ~3;

            vs_bitblt(vsapi->getWritePtr(dst, plane), vsapi->getStride(dst, plane), srcp, rowsize, rowsize, vsapi->getFrameHeight(dst, plane));
            srcp += vsapi->getFrameHeight(dst, plane) * rowsize;
        }
        break;
    }
}

class AVISource {
    IAVIReadHandler *pfile;
    IAVIReadStream *pvideo;
    HIC hic;
    VSVideoInfo vi[2];
    int numOutputs;
    BYTE* srcbuffer;
    int srcbuffer_size;
    BITMAPINFOHEADER* pbiSrc;
    BITMAPINFOHEADER biDst;
    bool ex;
    bool bIsType1;
    bool bInvertFrames;
    char buf[1024];
    BYTE* decbuf;

    const VSFrameRef *last_frame;
    const VSFrameRef *last_alpha_frame;
    int last_frame_no;

    LRESULT DecompressBegin(LPBITMAPINFOHEADER lpbiSrc, LPBITMAPINFOHEADER lpbiDst);
    LRESULT DecompressFrame(int n, bool preroll, bool &dropped_frame, VSFrameRef *frame, VSFrameRef *alpha, VSCore *core, const VSAPI *vsapi);

    void CheckHresult(HRESULT hr, const char* msg, VSCore *core, const VSAPI *vsapi);
    bool AttemptCodecNegotiation(DWORD fccHandler, BITMAPINFOHEADER* bmih);
    void LocateVideoCodec(const char fourCC[], VSCore *core, const VSAPI *vsapi);
    bool DecompressQuery(const VSFormat *format, bool forcedType, int bitcount, const int fourccs[], int nfourcc = 1) ;
public:

    enum {
        MODE_NORMAL = 0,
        MODE_AVIFILE,
        MODE_OPENDML,
        MODE_WAV
    };

    AVISource(const char filename[], const char pixel_type[],
        const char fourCC[], int mode, VSCore *core, const VSAPI *vsapi);  // mode: 0=detect, 1=avifile, 2=opendml
    ~AVISource();
    void CleanUp();
    const VSFrameRef *GetFrame(int n, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi);

    static void VS_CC create_AVISource(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
        try {
            const intptr_t mode = reinterpret_cast<intptr_t>(userData);
            int err;
            const char* path = vsapi->propGetData(in, "path", 0, nullptr);
            const char* pixel_type = vsapi->propGetData(in, "pixel_type", 0, &err);
            if (!pixel_type)
                pixel_type = "";
            const char* fourCC = vsapi->propGetData(in, "fourcc", 0, &err);
            if (!fourCC)
                fourCC = "";

            AVISource *avs = new AVISource(path, pixel_type, fourCC, static_cast<int>(mode), core, vsapi);
            vsapi->createFilter(in, out, "AVISource", filterInit, filterGetFrame, filterFree, fmUnordered, nfMakeLinear, static_cast<void *>(avs), core);

        } catch (std::runtime_error &e) {
            vsapi->setError(out, e.what());
        }
    }

    static void VS_CC filterInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
        AVISource *d = static_cast<AVISource *>(*instanceData);
        vsapi->setVideoInfo(d->vi, d->numOutputs, node);
    }

    static const VSFrameRef *VS_CC filterGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
        AVISource *d = static_cast<AVISource *>(*instanceData);

        if (activationReason == arInitial) {
            try {
                return d->GetFrame(n, frameCtx, core, vsapi);
            } catch (std::runtime_error &e) {
                vsapi->setFilterError(e.what(), frameCtx);
            }
        }
        return nullptr;
    }

    static void VS_CC filterFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
        AVISource *d = static_cast<AVISource *>(instanceData);
        delete d;
    }
};

bool AVISource::DecompressQuery(const VSFormat *format, bool forcedType, int bitcount, const int fourccs[], int nfourcc) {
    char fcc[5];
    fcc[4] = 0;
    for (int i = 0; i < nfourcc; i++) {
        *reinterpret_cast<int *>(fcc) = fourccs[i];
        biDst.biCompression = fourccs[0];
        vi[0].format = format;
        biDst.biSizeImage = ImageSize(vi, biDst.biCompression, bitcount);
        biDst.biBitCount = bitcount;

        if (ICERR_OK == ICDecompressQuery(hic, pbiSrc, &biDst)) {
            sprintf(buf, "AVISource: Opening as %s.\n", fcc);
            _RPT0(0, buf);
            return false;
        }
    }

    if (forcedType) {
        *reinterpret_cast<int *>(fcc) = fourccs[0];
        sprintf(buf, "AVISource: the video decompressor couldn't produce %s output", fcc);
        throw std::runtime_error(buf);
    }
    return true;
}


LRESULT AVISource::DecompressBegin(LPBITMAPINFOHEADER lpbiSrc, LPBITMAPINFOHEADER lpbiDst) {
    if (!ex) {
        LRESULT result = ICDecompressBegin(hic, lpbiSrc, lpbiDst);
        if (result != ICERR_UNSUPPORTED)
            return result;
        else
            ex = true;
        // and fall thru
    }
    return ICDecompressExBegin(hic, 0,
        lpbiSrc, 0, 0, 0, lpbiSrc->biWidth, lpbiSrc->biHeight,
        lpbiDst, 0, 0, 0, lpbiDst->biWidth, lpbiDst->biHeight);
}

LRESULT AVISource::DecompressFrame(int n, bool preroll, bool &dropped_frame, VSFrameRef *frame, VSFrameRef *alpha, VSCore *core, const VSAPI *vsapi) {
    _RPT2(0,"AVISource: Decompressing frame %d%s\n", n, preroll ? " (preroll)" : "");
    long bytes_read;
    if (!hic) {
        bytes_read = pbiSrc->biSizeImage;
        pvideo->Read(n, 1, decbuf, pbiSrc->biSizeImage, &bytes_read, nullptr);
        dropped_frame = !bytes_read;
        unpackframe(vi, frame, alpha, decbuf, bytes_read, pbiSrc->biCompression, pbiSrc->biBitCount, bInvertFrames, vsapi);
        return ICERR_OK;
    }
    bytes_read = srcbuffer_size;
    LRESULT err = pvideo->Read(n, 1, srcbuffer, srcbuffer_size, &bytes_read, nullptr);
    while (err == AVIERR_BUFFERTOOSMALL || (err == 0 && !srcbuffer)) {
        delete[] srcbuffer;
        pvideo->Read(n, 1, 0, srcbuffer_size, &bytes_read, nullptr);
        srcbuffer_size = bytes_read;
        srcbuffer = new BYTE[bytes_read + 16]; // Provide 16 hidden guard bytes for HuffYUV, Xvid, etc bug
        err = pvideo->Read(n, 1, srcbuffer, srcbuffer_size, &bytes_read, nullptr);
    }
    dropped_frame = !bytes_read;
    if (dropped_frame) return ICERR_OK;  // If frame is 0 bytes (dropped), return instead of attempt decompressing as Vdub.

    // Fill guard bytes with 0xA5's for Xvid bug
    memset(srcbuffer + bytes_read, 0xA5, 16);
    // and a Null terminator for good measure
    srcbuffer[bytes_read + 15] = 0;

    int flags = preroll ? ICDECOMPRESS_PREROLL : 0;
    flags |= dropped_frame ? ICDECOMPRESS_NULLFRAME : 0;
    flags |= !pvideo->IsKeyFrame(n) ? ICDECOMPRESS_NOTKEYFRAME : 0;
    pbiSrc->biSizeImage = bytes_read;
    LRESULT ret = (!ex ? ICDecompress(hic, flags, pbiSrc, srcbuffer, &biDst, decbuf)
        : ICDecompressEx(hic, flags, pbiSrc, srcbuffer, 0, 0, vi[0].width, vi[0].height, &biDst, decbuf, 0, 0, vi[0].width, vi[0].height));

    if (ret != ICERR_OK)
        return ret;

    unpackframe(vi, frame, alpha, decbuf, 0, biDst.biCompression, biDst.biBitCount, bInvertFrames, vsapi);

    if (pvideo->IsKeyFrame(n)) {
        vsapi->propSetData(vsapi->getFramePropsRW(frame), "_PictType", "I", 1, paAppend);
        if (alpha)
            vsapi->propSetData(vsapi->getFramePropsRW(alpha), "_PictType", "I", 1, paAppend);
    } else {
        vsapi->propSetData(vsapi->getFramePropsRW(frame), "_PictType", "P", 1, paAppend);
        if (alpha)
            vsapi->propSetData(vsapi->getFramePropsRW(alpha), "_PictType", "P", 1, paAppend);
    }

    return ICERR_OK;
}


void AVISource::CheckHresult(HRESULT hr, const char* msg, VSCore *core, const VSAPI *vsapi) {
    if (SUCCEEDED(hr)) return;
    char buf2[1024] = {0};
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, hr, 0, buf, 1024, nullptr))
        wsprintfA(buf2, "error code 0x%x", hr);
    sprintf(buf, "AVISource: %s:\n%s", msg, buf2);
    throw std::runtime_error(buf);
}


// taken from VirtualDub
bool AVISource::AttemptCodecNegotiation(DWORD fccHandler, BITMAPINFOHEADER* bmih) {

    // Try the handler specified in the file first.  In some cases, it'll
    // be wrong or missing.

    if (fccHandler)
        hic = ICOpen(ICTYPE_VIDEO, fccHandler, ICMODE_DECOMPRESS);

    if (!hic || ICERR_OK!=ICDecompressQuery(hic, bmih, nullptr)) {
        if (hic)
            ICClose(hic);

        // Pick a handler based on the biCompression field instead.

        hic = ICOpen(ICTYPE_VIDEO, bmih->biCompression, ICMODE_DECOMPRESS);

        if (!hic || ICERR_OK!=ICDecompressQuery(hic, bmih, nullptr)) {
            if (hic)
                ICClose(hic);

            // This never seems to work...

            hic = ICLocate(ICTYPE_VIDEO, 0, bmih, nullptr, ICMODE_DECOMPRESS);
        }
    }

    return !!hic;
}


void AVISource::LocateVideoCodec(const char fourCC[], VSCore *core, const VSAPI *vsapi) {
    VDAVIStreamInfo asi;
    CheckHresult(pvideo->Info(&asi), "couldn't get video info", core, vsapi);
    long size = sizeof(BITMAPINFOHEADER);

    // Read video format.  If it's a
    // type-1 DV, we're going to have to fake it.

    if (bIsType1) {
        pbiSrc = (BITMAPINFOHEADER *)malloc(size);
        if (!pbiSrc)
            throw std::runtime_error("AviSource: Could not allocate BITMAPINFOHEADER.");

        pbiSrc->biSize      = sizeof(BITMAPINFOHEADER);
        pbiSrc->biWidth     = 720;

        if (asi.dwRate > asi.dwScale*26i64)
            pbiSrc->biHeight      = 480;
        else
            pbiSrc->biHeight      = 576;

        pbiSrc->biPlanes      = 1;
        pbiSrc->biBitCount    = 24;
        pbiSrc->biCompression   = 'dsvd';
        pbiSrc->biSizeImage   = asi.dwSuggestedBufferSize;
        pbiSrc->biXPelsPerMeter = 0;
        pbiSrc->biYPelsPerMeter = 0;
        pbiSrc->biClrUsed     = 0;
        pbiSrc->biClrImportant  = 0;

    } else {
        CheckHresult(pvideo->ReadFormat(0, 0, &size), "couldn't get video format size", core, vsapi);
        pbiSrc = (LPBITMAPINFOHEADER)malloc(size);
        CheckHresult(pvideo->ReadFormat(0, pbiSrc, &size), "couldn't get video format", core, vsapi);
    }

    vi[0].width = pbiSrc->biWidth;
    vi[0].height = abs(pbiSrc->biHeight);
    vi[0].fpsNum = asi.dwRate;
    vi[0].fpsDen = asi.dwScale;
    vi[0].numFrames = asi.dwLength;

    // try the requested decoder, if specified
    if (fourCC != nullptr && strlen(fourCC) == 4) {
        DWORD fcc = fourCC[0] | (fourCC[1] << 8) | (fourCC[2] << 16) | (fourCC[3] << 24);
        asi.fccHandler = pbiSrc->biCompression = fcc;
    }

    // see if we can handle the video format directly
    if (pbiSrc->biCompression == VS_FCC('YUY2')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV422P8, core);
    } else if (pbiSrc->biCompression == VS_FCC('YV12') || pbiSrc->biCompression == VS_FCC('I420')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV420P8, core);
    } else if (pbiSrc->biCompression == BI_RGB && pbiSrc->biBitCount == 32) {
        vi[0].format = vsapi->getFormatPreset(pfRGB24, core);
        numOutputs = 2;
        vi[1] = vi[0];
        vi[1].format = vsapi->getFormatPreset(pfGray8, core);
        if (pbiSrc->biHeight > 0)
            bInvertFrames = true;
    } else if (pbiSrc->biCompression == BI_RGB && pbiSrc->biBitCount == 24) {
        vi[0].format = vsapi->getFormatPreset(pfRGB24, core);
        if (pbiSrc->biHeight > 0)
            bInvertFrames = true;
    } else if (pbiSrc->biCompression == VS_FCC('b48r')) {
        vi[0].format = vsapi->getFormatPreset(pfRGB48, core);
    } else if (pbiSrc->biCompression == VS_FCC('b64a')) {
        vi[0].format = vsapi->getFormatPreset(pfRGB48, core);
    } else if (pbiSrc->biCompression == VS_FCC('GREY') || pbiSrc->biCompression == VS_FCC('Y800') || pbiSrc->biCompression == VS_FCC('Y8  ')) {
        vi[0].format = vsapi->getFormatPreset(pfGray8, core);
    } else if (pbiSrc->biCompression == VS_FCC('YV24')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV444P8, core);
    } else if (pbiSrc->biCompression == VS_FCC('YV16')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV422P8, core);
    } else if (pbiSrc->biCompression == VS_FCC('Y41B')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV411P8, core);
    } else if (pbiSrc->biCompression == VS_FCC('P010')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV420P10, core);
    } else if (pbiSrc->biCompression == VS_FCC('P016')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV420P16, core);
    } else if (pbiSrc->biCompression == VS_FCC('P210')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV422P10, core);
    } else if (pbiSrc->biCompression == VS_FCC('P216')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV422P16, core);
    } else if (pbiSrc->biCompression == VS_FCC('v210')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV422P10, core);
    } else if (pbiSrc->biCompression == VS_FCC('Y416')) {
        vi[0].format = vsapi->getFormatPreset(pfYUV444P16, core);

        // otherwise, find someone who will decompress it
    } else {
        switch(pbiSrc->biCompression) {
        case VS_FCC('MP43'):    // Microsoft MPEG-4 V3
        case VS_FCC('DIV3'):    // "DivX Low-Motion" (4.10.0.3917)
        case VS_FCC('DIV4'):    // "DivX Fast-Motion" (4.10.0.3920)
        case VS_FCC('AP41'):    // "AngelPotion Definitive" (4.0.00.3688)
            if (AttemptCodecNegotiation(asi.fccHandler, pbiSrc)) return;
            pbiSrc->biCompression = VS_FCC('MP43');
            if (AttemptCodecNegotiation(asi.fccHandler, pbiSrc)) return;
            pbiSrc->biCompression = VS_FCC('DIV3');
            if (AttemptCodecNegotiation(asi.fccHandler, pbiSrc)) return;
            pbiSrc->biCompression = VS_FCC('DIV4');
            if (AttemptCodecNegotiation(asi.fccHandler, pbiSrc)) return;
            pbiSrc->biCompression = VS_FCC('AP41');
        default:
            if (AttemptCodecNegotiation(asi.fccHandler, pbiSrc)) return;
        }
        sprintf(buf, "AVISource: couldn't locate a decompressor for fourcc %c%c%c%c",
            asi.fccHandler, asi.fccHandler>>8, asi.fccHandler>>16, asi.fccHandler>>24);
        throw std::runtime_error(buf);
    }
}


AVISource::AVISource(const char filename[], const char pixel_type[], const char fourCC[], int mode, VSCore *core, const VSAPI *vsapi)
    : numOutputs(1), last_frame_no(-1), last_frame(nullptr), last_alpha_frame(nullptr), srcbuffer(nullptr), srcbuffer_size(0), ex(false), pbiSrc(nullptr),
    pvideo(nullptr), pfile(nullptr), bIsType1(false), hic(0), bInvertFrames(false), decbuf(nullptr)  {
    memset(vi, 0, sizeof(vi));

    AVIFileInit();
    try {

        std::vector<wchar_t> wfilename;
        wfilename.resize(MultiByteToWideChar(CP_UTF8, 0, filename, -1, nullptr, 0));
        MultiByteToWideChar(CP_UTF8, 0, filename, -1, wfilename.data(), static_cast<int>(wfilename.size()));

        if (mode == MODE_NORMAL) {
            // if it looks like an AVI file, open in OpenDML mode; otherwise AVIFile mode
            HANDLE h = CreateFile(wfilename.data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                sprintf(buf, "AVISource autodetect: couldn't open file '%s'\nError code: %d", filename, GetLastError());
                throw std::runtime_error(buf);
            }
            unsigned int buf[3];
            DWORD bytes_read;
            if (ReadFile(h, buf, 12, &bytes_read, nullptr) && bytes_read == 12 && buf[0] == 'FFIR' && buf[2] == ' IVA')
                mode = MODE_OPENDML;
            else
                mode = MODE_AVIFILE;
            CloseHandle(h);
        }

        if (mode == MODE_AVIFILE || mode == MODE_WAV) {    // AVIFile mode
            PAVIFILE paf;
            if (FAILED(AVIFileOpen(&paf, wfilename.data(), OF_READ, 0))) {
                sprintf(buf, "AVIFileSource: couldn't open file '%s'", filename);
                throw std::runtime_error(buf);
            }

            pfile = CreateAVIReadHandler(paf);
        } else {              // OpenDML mode
            pfile = CreateAVIReadHandler(wfilename.data());
        }

        if (mode != MODE_WAV) { // check for video stream
            pvideo = pfile->GetStream(streamtypeVIDEO, 0);

            if (!pvideo) { // Attempt DV type 1 video.
                pvideo = pfile->GetStream('svai', 0);
                bIsType1 = true;
            }

            if (pvideo) {
                LocateVideoCodec(fourCC, core, vsapi);
                if (hic) {
                    bool forcedType = !(pixel_type[0] == 0);
                    
                    bool fY8    = lstrcmpiA(pixel_type, "Y8"   ) == 0 || pixel_type[0] == 0;
                    bool fYV12  = lstrcmpiA(pixel_type, "YV12" ) == 0 || pixel_type[0] == 0;
                    bool fYV16  = lstrcmpiA(pixel_type, "YV16" ) == 0 || pixel_type[0] == 0;
                    bool fYV24  = lstrcmpiA(pixel_type, "YV24" ) == 0 || pixel_type[0] == 0;
                    bool fYV411 = lstrcmpiA(pixel_type, "YV411") == 0 || pixel_type[0] == 0;
                    bool fYUY2  = lstrcmpiA(pixel_type, "YUY2" ) == 0 || pixel_type[0] == 0;
                    bool fRGB32 = lstrcmpiA(pixel_type, "RGB32") == 0 || pixel_type[0] == 0;
                    bool fRGB24 = lstrcmpiA(pixel_type, "RGB24") == 0 || pixel_type[0] == 0;
                    bool fRGB48 = lstrcmpiA(pixel_type, "RGB48") == 0 || pixel_type[0] == 0;
                    bool fRGB64 = lstrcmpiA(pixel_type, "RGB64") == 0 || pixel_type[0] == 0;
                    bool fP010  = lstrcmpiA(pixel_type, "P010")  == 0 || pixel_type[0] == 0;
                    bool fP016  = lstrcmpiA(pixel_type, "P016")  == 0 || pixel_type[0] == 0;
                    bool fP210  = lstrcmpiA(pixel_type, "P210")  == 0 || pixel_type[0] == 0;
                    bool fP216  = lstrcmpiA(pixel_type, "P216")  == 0 || pixel_type[0] == 0;
                    bool fY416  = lstrcmpiA(pixel_type, "Y416")  == 0 || pixel_type[0] == 0;
                    bool fv210  = lstrcmpiA(pixel_type, "v210")  == 0 || pixel_type[0] == 0;

                    if (!(fY8 || fYV12 || fYV16 || fYV24 || fYV411 || fYUY2 || fRGB32 || fRGB24 || fRGB48 || fRGB64 || fP010 || fP016 || fP210 || fP216 || fY416 || fv210))
                        throw std::runtime_error("AVISource: requested format must be one of YV24, YV16, YV12, YV411, YUY2, Y8, RGB24, RGB32, RGB48, RGB64, P010, P016, P210, P216, Y416, v210");

                    // try to decompress to YV12, YV411, YV16, YV24, YUY2, Y8, RGB32, and RGB24 in turn
                    memset(&biDst, 0, sizeof(BITMAPINFOHEADER));
                    biDst.biSize = sizeof(BITMAPINFOHEADER);
                    biDst.biWidth = vi[0].width;
                    biDst.biHeight = vi[0].height;
                    biDst.biPlanes = 1;
                    bool bOpen = true;
                    
                    const int fccyv24[]  = { VS_FCC('YV24') };
                    const int fccyv16[]  = { VS_FCC('YV16') };
                    const int fccyv12[]  = { VS_FCC('YV12'), VS_FCC('I420') };
                    const int fccyv411[] = { VS_FCC('Y41B') };
                    const int fccyuy2[]  = { VS_FCC('YUY2') };
                    const int fccrgb[]   = {BI_RGB};
                    const int fccb48r[]  = { VS_FCC('b48r') };
                    const int fccb64a[]  = { VS_FCC('b64a') };
                    const int fccy8[]    = { VS_FCC('Y800'), VS_FCC('Y8  '), VS_FCC('GREY') };
                    const int fccp010[]  = { VS_FCC('P010') };
                    const int fccp016[]  = { VS_FCC('P016') };
                    const int fccp210[]  = { VS_FCC('P210') };
                    const int fccp216[]  = { VS_FCC('P216') };
                    const int fccy416[]  = { VS_FCC('Y416') };
                    const int fccv210[]  = { VS_FCC('v210') };

                    if (fYV24 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV444P8, core), forcedType, 24, fccyv24);
                    if (fYV16 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV422P8, core), forcedType, 16, fccyv16);
                    if (fYV12 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV420P8, core), forcedType, 12, fccyv12, sizeof(fccyv12)/sizeof(fccyv12[0]));
                    if (fYV411 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV411P8, core), forcedType, 16, fccyv411);
                    if (fYUY2 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV422P8, core), forcedType, 16, fccyuy2);
                    if (fRGB32 && bOpen) {
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfRGB24, core), forcedType, 32, fccrgb);
                        if (!bOpen) {
                            numOutputs = 2;
                            vi[1] = vi[0];
                            vi[1].format = vsapi->getFormatPreset(pfGray8, core);
                        }
                    }
                    if (fRGB24 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfRGB24, core), forcedType, 24, fccrgb);
                    if (fRGB48 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfRGB48, core), forcedType, 48, fccb48r);
                    if (fRGB64 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfRGB48, core), forcedType, 64, fccb64a);
                    if (fY8 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfGray8, core), forcedType, 8, fccy8, sizeof(fccy8)/sizeof(fccy8[0]));
                    if (fP010 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV420P10, core), forcedType, 24, fccp010);
                    if (fP016 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV420P16, core), forcedType, 24, fccp016);
                    if (fP210 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV422P10, core), forcedType, 24, fccp210);
                    if (fP216 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV422P16, core), forcedType, 24, fccp216);
                    if (fY416 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV444P16, core), forcedType, 32, fccy416);
                    if (fv210 && bOpen)
                        bOpen = DecompressQuery(vsapi->getFormatPreset(pfYUV422P10, core), forcedType, 20, fccv210);

                    // No takers!
                    if (bOpen)
                        throw std::runtime_error("AviSource: Could not open video stream in any supported format.");

                    DecompressBegin(pbiSrc, &biDst);
                }
            } else {
                throw std::runtime_error("AviSource: Could not locate video stream.");
            }
        }

        // try to decompress frame 0 if not audio only.

        bool dropped_frame = false;

        if (mode != MODE_WAV) {
            decbuf = vs_aligned_malloc<BYTE>(hic ? biDst.biSizeImage : pbiSrc->biSizeImage, 32);
            int keyframe = pvideo->NearestKeyFrame(0);
            VSFrameRef *frame = vsapi->newVideoFrame(vi[0].format, vi[0].width, vi[0].height, nullptr, core);
            VSFrameRef *alpha_frame = nullptr;
            if (numOutputs == 2)
                alpha_frame = vsapi->newVideoFrame(vi[1].format, vi[1].width, vi[1].height, nullptr, core);
            LRESULT error = DecompressFrame(keyframe, false, dropped_frame, frame, alpha_frame, core, vsapi);
            if (error != ICERR_OK)   // shutdown, if init not succesful.
                throw std::runtime_error("AviSource: Could not decompress frame 0");

            // Cope with dud AVI files that start with drop
            // frames, just return the first key frame
            if (dropped_frame) {
                keyframe = pvideo->NextKeyFrame(0);
                error = DecompressFrame(keyframe, false, dropped_frame, frame, alpha_frame, core, vsapi);
                if (error != ICERR_OK) {   // shutdown, if init not succesful.
                    sprintf(buf, "AviSource: Could not decompress first keyframe %d", keyframe);
                    throw std::runtime_error(buf);
                }
            }

            last_frame_no=0;
            last_frame=frame;
            last_alpha_frame = alpha_frame;
        }
    }
    catch (std::runtime_error) {
        AVISource::CleanUp();
        throw;
    }
}

AVISource::~AVISource() {
    AVISource::CleanUp();
}

void AVISource::CleanUp() {
    if (hic) {
        !ex ? ICDecompressEnd(hic) : ICDecompressExEnd(hic);
        ICClose(hic);
    }
    if (pvideo) delete pvideo;
    if (pfile)
        pfile->Release();
    AVIFileExit();
    if (pbiSrc)
        free(pbiSrc);
    if (srcbuffer)
        delete[] srcbuffer;
    vs_aligned_free(decbuf);
    // fixme
    //vsapi->freeFrame(last_frame);
}

const VSFrameRef *AVISource::GetFrame(int n, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    n = min(max(n, 0), vi[0].numFrames - 1);
    bool dropped_frame = false;
    if (n != last_frame_no || !last_frame) {
        // find the last keyframe
        int keyframe = pvideo->NearestKeyFrame(n);
        // maybe we don't need to go back that far
        if (last_frame_no < n && last_frame_no >= keyframe)
            keyframe = last_frame_no + 1;
        if (keyframe < 0) keyframe = 0;

        bool frameok = false;
        VSFrameRef *frame = vsapi->newVideoFrame(vi[0].format, vi[0].width, vi[0].height, nullptr, core);
        VSFrameRef *alpha_frame = nullptr;
        if (numOutputs == 2)
            alpha_frame = vsapi->newVideoFrame(vi[1].format, vi[1].width, vi[1].height, nullptr, core);
        bool not_found_yet;
        do {
            not_found_yet = false;
            for (VDPosition i = keyframe; i <= n; ++i) {
                LRESULT error = DecompressFrame(i, i != n, dropped_frame, frame, alpha_frame, core, vsapi);
                if ((!dropped_frame) && (error == ICERR_OK))
                    frameok = true;   // Better safe than sorry
            }
            last_frame_no = n;

            if (!last_frame && !frameok) {  // Last keyframe was not valid.
                const VDPosition key_pre = keyframe;
                keyframe = pvideo->NearestKeyFrame(keyframe - 1);
                if (keyframe < 0) keyframe = 0;
                if (keyframe == key_pre) {
                    sprintf(buf, "AVISource: could not find valid keyframe for frame %d.", n);
                    throw std::runtime_error(buf);
                }

                not_found_yet = true;
            }
        } while (not_found_yet);

        if (frameok) {
            vsapi->freeFrame(last_frame);
            vsapi->freeFrame(last_alpha_frame);
            last_frame = frame;
            last_alpha_frame = alpha_frame;
        }
    }

    if (!last_frame) {
        sprintf(buf, "AVISource: failed to decode frame %d.", n);
        throw std::runtime_error(buf);
    }

    int o = vsapi->getOutputIndex(frameCtx);
    if (vsapi->getOutputIndex(frameCtx) == 0)
        return vsapi->cloneFrameRef(last_frame);
    else
        return vsapi->cloneFrameRef(last_alpha_frame);
}

//////////////////////////////////////////
// Init

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("com.vapoursynth.avisource", "avisource", "VapourSynth AVISource Port", VAPOURSYNTH_API_VERSION, 1, plugin);
    const char *args = "path:data[];pixel_type:data:opt;fourcc:data:opt;";
    registerFunc("AVISource", args, AVISource::create_AVISource, reinterpret_cast<void *>(AVISource::MODE_NORMAL), plugin);
    registerFunc("AVIFileSource", args, AVISource::create_AVISource, reinterpret_cast<void *>(AVISource::MODE_AVIFILE), plugin);
    registerFunc("OpenDMLSource", args, AVISource::create_AVISource, reinterpret_cast<void *>(AVISource::MODE_OPENDML), plugin);
}
