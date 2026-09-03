/*
 * Copyright (C) 2019 Microsoft Corporation.
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
#include "WebPageInspectorEmulationAgent.h"

#include "DrawingAreaProxyCoordinatedGraphics.h"
#include "WebPageProxy.h"

#if ENABLE(WPE_PLATFORM)
#include <wpe/wpe-platform.h>
#endif

namespace WebKit {

void WebPageInspectorEmulationAgent::platformSetSize(int width, int height, Function<void (const String& error)>&& callback)
{
    WebCore::IntSize viewSize(width, height);
    if (m_page.viewSize() == viewSize) {
        callback(String());
        return;
    }

    auto waitForSizeUpdate = [this, callback = WTF::move(callback)]() mutable {
        if (auto* drawingArea = static_cast<DrawingAreaProxyCoordinatedGraphics*>(m_page.drawingArea())) {
            drawingArea->waitForSizeUpdate([callback = WTF::move(callback)](const DrawingAreaProxyCoordinatedGraphics&) mutable {
                callback(String());
            });
        } else
            callback(String());
    };

#if ENABLE(WPE_PLATFORM)
    // WPEPlatform path: there is no libwpe view backend, so resize the WPEView's toplevel, which
    // propagates the new size to the web process via WKWPE::ViewPlatform's "resized" signal. Note
    // this only works reliably on displays where the client controls the toplevel size (e.g. the
    // headless display); on Wayland the compositor's configure event may override the request.
    if (auto* wpeView = m_page.wpeView()) {
        if (auto* toplevel = wpe_view_get_toplevel(wpeView))
            wpe_toplevel_resize(toplevel, viewSize.width(), viewSize.height());
        waitForSizeUpdate();
        return;
    }
#endif
    waitForSizeUpdate();
}

} // namespace WebKit
