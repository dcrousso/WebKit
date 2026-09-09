/*
 * Copyright (C) 2020 Microsoft Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "InspectorScreencastAgent.h"

#include "PageClient.h"
#include "WebPageInspectorController.h"
#include "WebPageProxy.h"
#include "WebsiteDataStore.h"
#include <pal/crypto/CryptoDigest.h>
#include <JavaScriptCore/InspectorFrontendRouter.h>
#include <WebCore/NotImplemented.h>
#include <wtf/Compiler.h>
#include <wtf/RunLoop.h>
#include <wtf/UUID.h>
#include <wtf/text/Base64.h>

#if USE(SKIA)
#include "DrawingAreaProxyCoordinatedGraphics.h"
#include "DrawingAreaProxy.h"
#include <skia/core/SkBitmap.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPixmap.h>
#include <skia/core/SkData.h>
#include <skia/core/SkStream.h>
#include <skia/encode/SkJpegEncoder.h>
#endif

#if PLATFORM(MAC)
#include <WebCore/ImageUtilities.h>
#endif

#if PLATFORM(WIN)
#include "DrawingAreaProxyWC.h"
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace WebKit {

const int kMaxFramesInFlight = 1;

using namespace Inspector;

WTF_MAKE_TZONE_ALLOCATED_IMPL(InspectorScreencastAgent);

InspectorScreencastAgent::InspectorScreencastAgent(BackendDispatcher& backendDispatcher, Inspector::FrontendRouter& frontendRouter, WebPageProxy& page)
    : InspectorAgentBase("Screencast"_s)
    , m_frontendDispatcher(makeUnique<ScreencastFrontendDispatcher>(frontendRouter))
    , m_backendDispatcher(ScreencastBackendDispatcher::create(backendDispatcher, this))
    , m_page(page)
{
}

InspectorScreencastAgent::~InspectorScreencastAgent()
{
}

void InspectorScreencastAgent::didCreateFrontendAndBackend()
{
}

void InspectorScreencastAgent::willDestroyFrontendAndBackend(DisconnectReason)
{
}

#if USE(SKIA)
void InspectorScreencastAgent::didPaint(sk_sp<SkImage>&& surface)
{
    if (!m_screencast)
        return;

    if (m_screencastFramesInFlight > kMaxFramesInFlight)
        return;

    MonotonicTime timestamp = MonotonicTime::now();
    sk_sp<SkImage> image(surface);

    // Get actual image size (in device pixels).
    WebCore::IntSize displaySize(image->width(), image->height());
    WebCore::IntSize drawingAreaSize = m_page.drawingArea()->size();
    drawingAreaSize.scale(m_page.deviceScaleFactor());
    if (drawingAreaSize != displaySize)
        return;

    {
        SkPixmap pixmap;
        if (!image->peekPixels(&pixmap)) {
            fprintf(stderr, "Failed to peek pixels from SkImage to compute hash\n");
            return;
        }
        // Do not send the same frame over and over.
        size_t len = pixmap.computeByteSize();
        auto cryptoDigest = PAL::Crypto::CryptoDigest::create(PAL::Crypto::CryptoDigest::Algorithm::SHA_1);
        cryptoDigest->addBytes(std::span(reinterpret_cast<const unsigned char*>(pixmap.addr()), len));
        auto digest = cryptoDigest->computeHash();
        if (m_lastFrameDigest == digest)
            return;
        m_lastFrameDigest = digest;
    }

    // Scale image to fit width / height
    double scale = std::min(m_screencastWidth / displaySize.width(), m_screencastHeight / displaySize.height());
    if (scale < 1) {
        SkBitmap dstBitmap;
        dstBitmap.allocPixels(SkImageInfo::MakeN32Premul(displaySize.width() * scale, displaySize.height() * scale));
        SkCanvas canvas(dstBitmap);
        canvas.scale(scale, scale);
        canvas.drawImage(image, 0, 0);
        image = dstBitmap.asImage();
    }

    SkPixmap pixmap;
    if (!image->peekPixels(&pixmap)) {
        fprintf(stderr, "Failed to peek pixels from SkImage for JPEG encoding\n");
        return;
    }

    SkJpegEncoder::Options options;
    options.fQuality = 90;
    SkDynamicMemoryWStream stream;
    if (!SkJpegEncoder::Encode(&stream, pixmap, options)) {
        fprintf(stderr, "Failed to encode image to JPEG\n");
        return;
    }
    sk_sp<SkData> jpegData = stream.detachAsData();
    String result = base64EncodeToString(std::span(reinterpret_cast<const unsigned char*>(jpegData->data()), jpegData->size()));
    ++m_screencastFramesInFlight;
    m_frontendDispatcher->screencastFrame(result, timestamp.secondsSinceEpoch().value(), displaySize.width(), displaySize.height());
}
#endif

Inspector::Protocol::ErrorStringOr<int /* generation */> InspectorScreencastAgent::startScreencast(int width, int height, int toolbarHeight, int quality)
{
    if (m_screencast)
        return makeUnexpected("Already screencasting"_s);

    m_screencast = true;
    m_screencastWidth = width;
    m_screencastHeight = height;
    m_screencastQuality = quality;
    m_screencastToolbarHeight = toolbarHeight;
    ++m_screencastGeneration;
    kickFramesStarted();
    return m_screencastGeneration;
}

Inspector::Protocol::ErrorStringOr<void> InspectorScreencastAgent::screencastFrameAck(int generation)
{
    if (!m_screencast)
        return makeUnexpected("Not screencasting"_s);

    if (m_screencastGeneration != generation)
        return { };

    --m_screencastFramesInFlight;
    return { };
}

Inspector::Protocol::ErrorStringOr<void> InspectorScreencastAgent::stopScreencast()
{
    if (!m_screencast)
        return makeUnexpected("Not screencasting"_s);

    m_screencast = false;
    m_framesAreGoing = false;
    m_screencastFramesInFlight = 0;
    m_lastFrameDigest.clear();
    return { };
}

void InspectorScreencastAgent::kickFramesStarted()
{
    if (!m_framesAreGoing) {
        m_framesAreGoing = true;
#if !PLATFORM(WPE)
        scheduleFrameEncoding();
#endif
    }
    m_page.updateRenderingWithForcedRepaint([] { });
}

#if !PLATFORM(WPE)
void InspectorScreencastAgent::scheduleFrameEncoding()
{
    if (!m_screencast)
        return;

    const int fps = 25;
    RunLoop::mainSingleton().dispatchAfter(Seconds(1.0 / fps), [agent = WeakPtr { this }]() mutable {
        if (!agent)
            return;
        if (!agent->m_page.hasPageClient())
            return;

        agent->encodeFrame();
        agent->scheduleFrameEncoding();
    });
}

void InspectorScreencastAgent::encodeFrame()
{
    if (!m_screencast || m_screencastFramesInFlight > kMaxFramesInFlight)
        return;

#if PLATFORM(MAC)
    RetainPtr<CGImageRef> imageRef = m_page.pageClient()->takeSnapshotForAutomation();
    MonotonicTime timestamp = MonotonicTime::now();
    CGImage* imagePtr = imageRef.get();
    WebCore::IntSize imageSize(CGImageGetWidth(imagePtr), CGImageGetHeight(imagePtr));
    WebCore::IntSize displaySize = imageSize;
    displaySize.contract(0, m_screencastToolbarHeight);
    double scale = std::min(m_screencastWidth / displaySize.width(), m_screencastHeight / displaySize.height());
    RetainPtr<CGImageRef> transformedImageRef;
    if (scale < 1 || m_screencastToolbarHeight) {
        WebCore::IntSize screencastSize = displaySize;
        WebCore::IntSize scaledImageSize = imageSize;
        if (scale < 1) {
            screencastSize.scale(scale);
            scaledImageSize.scale(scale);
        }
        RetainPtr<CGColorSpaceRef> colorSpace = adoptCF(CGColorSpaceCreateDeviceRGB());
        RetainPtr<CGContextRef> context = adoptCF(CGBitmapContextCreate(nullptr, screencastSize.width(), screencastSize.height(), 8, 4 * screencastSize.width(), colorSpace.get(), (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host));
        CGContextDrawImage(context.get(), CGRectMake(0, 0, scaledImageSize.width(), scaledImageSize.height()), imagePtr);
        transformedImageRef = adoptCF(CGBitmapContextCreateImage(context.get()));
        imagePtr = transformedImageRef.get();
    }
    auto data = WebCore::encodeData(imagePtr, "image/jpeg"_s, m_screencastQuality * 0.1);

    // Do not send the same frame over and over.
    auto cryptoDigest = PAL::Crypto::CryptoDigest::create(PAL::Crypto::CryptoDigest::Algorithm::SHA_1);
    cryptoDigest->addBytes(std::span(data.mutableSpan().data(), data.size()));
    auto digest = cryptoDigest->computeHash();
    if (m_lastFrameDigest != digest) {
        String base64Data = base64EncodeToString(data);
        ++m_screencastFramesInFlight;
        m_frontendDispatcher->screencastFrame(base64Data, timestamp.secondsSinceEpoch().value(), displaySize.width(), displaySize.height());
        m_lastFrameDigest = digest;
    }
#elif PLATFORM(GTK)
    if (auto* drawingArea = m_page.drawingArea())
        static_cast<DrawingAreaProxyCoordinatedGraphics*>(drawingArea)->captureFrame();
#elif PLATFORM(WIN)
    if (auto* drawingArea = m_page.drawingArea())
        static_cast<DrawingAreaProxyWC*>(drawingArea)->captureFrame();
#endif
}
#endif

} // namespace WebKit

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
