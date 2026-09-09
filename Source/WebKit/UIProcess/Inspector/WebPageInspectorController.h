/*
 * Copyright (C) 2018 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "InspectorTargetProxy.h"
#include "ProvisionalPageProxy.h"
#include "UIProcess/WebFrameProxy.h"
#include "ProcessTerminationReason.h"
#include <JavaScriptCore/InspectorAgentRegistry.h>
#include <JavaScriptCore/InspectorTargetAgent.h>
#include <WebCore/FrameIdentifier.h>
#include <WebCore/NavigationIdentifier.h>
#include <WebCore/PageIdentifier.h>
#include <WebCore/ProcessIdentifier.h>
#include <wtf/CheckedRef.h>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/WTFString.h>
#include <wtf/URL.h>

#if USE(SKIA)
#include <skia/core/SkData.h>
#include <skia/core/SkImage.h>
#endif

namespace Inspector {
class BackendDispatcher;
class FrontendChannel;
class FrontendRouter;
class ProxyingNetworkAgent;
class ProxyingPageAgent;
class InspectorTarget;
}

namespace WebCore {
class ResourceError;
class ResourceRequest;
enum class PolicyAction : uint8_t;
struct WindowFeatures;
}

namespace PAL {
class SessionID;
}

namespace WebKit {

class InspectorBrowserAgent;
class ProvisionalPageProxy;
struct WebPageAgentContext;

class InspectorScreencastAgent;
class WebFrameProxy;
class WebPageInspectorEmulationAgent;
class WebPageInspectorInputAgent;

class WebPageInspectorControllerObserver {
public:
    virtual void didCreateInspectorController(WebPageProxy&) = 0;
    virtual void willDestroyInspectorController(WebPageProxy&) = 0;
    virtual void didFailProvisionalLoad(WebPageProxy&, WebCore::NavigationIdentifier, const String& error) = 0;
    virtual void willCreateNewPage(WebPageProxy&, const WebCore::WindowFeatures&, const URL&) = 0;

protected:
    virtual ~WebPageInspectorControllerObserver() = default;
};

class WebPageInspectorController {
    WTF_MAKE_TZONE_ALLOCATED(WebPageInspectorController);
    WTF_MAKE_NONCOPYABLE(WebPageInspectorController);
public:
    WebPageInspectorController(WebPageProxy&);
    ~WebPageInspectorController();

    void init();
    void didInitializeWebPage();

    static void setObserver(WebPageInspectorControllerObserver*);
    static WebPageInspectorControllerObserver* observer();

    void pageClosed();
    bool pageCrashed(ProcessTerminationReason);

    void willCreateNewPage(const WebCore::WindowFeatures&, const URL&);

    void didShowPage();

    void didProcessAllPendingKeyboardEvents();
    void didProcessAllPendingMouseEvents();
    void didProcessAllPendingWheelEvents();

    bool hasLocalFrontend() const;

    void connectFrontend(Inspector::FrontendChannel&, bool isAutomaticInspection = false, bool immediatelyPause = false);
    void disconnectFrontend(Inspector::FrontendChannel&);
    void disconnectAllFrontends();

    void dispatchMessageFromFrontend(const String& message);

#if ENABLE(REMOTE_INSPECTOR)
    void setIndicating(bool);
#endif
#if USE(SKIA)
    void didPaint(sk_sp<SkImage>&&);
#endif
    bool screencastActive() const;
    using NavigationHandler = Function<void(const String&, Markable<WebCore::NavigationIdentifier>)>;
    void navigate(WebCore::ResourceRequest&&, WebFrameProxy*, NavigationHandler&&);
    void didReceivePolicyDecision(WebCore::PolicyAction action, std::optional<WebCore::NavigationIdentifier> navigationID);

    void didDestroyNavigation(WebCore::NavigationIdentifier navigationID);

    void didFailProvisionalLoadForFrame(WebCore::NavigationIdentifier navigationID, const WebCore::ResourceError& error);

    void sendMessageToInspectorFrontend(const String& targetId, const String& message);

    void setPauseOnStart(bool);

    bool shouldPauseLoadRequest() const;
    bool shouldPauseInInspectorWhenShown() const;
    void setContinueLoadingCallback(WTF::Function<void()>&&);

    bool shouldPauseLoadingForPage(const ProvisionalPageProxy&) const;
    void setContinueLoadingCallbackForPage(const ProvisionalPageProxy&, WTF::Function<void()>&&);
    bool shouldPauseLoadingForFrame(const ProvisionalFrameProxy&) const;
    void setContinueLoadingCallbackForFrame(const ProvisionalFrameProxy&, WTF::Function<void()>&&);

    void didCreateProvisionalPage(ProvisionalPageProxy&, WebCore::FrameIdentifier mainFrameID, WebProcessProxy& mainFrameProcess);
    void willDestroyProvisionalPage(const ProvisionalPageProxy&, WebCore::FrameIdentifier mainFrameID, WebCore::ProcessIdentifier mainFrameProcessID);
    void didCommitProvisionalPage(std::optional<WebCore::FrameIdentifier> oldMainFrameID, WebCore::ProcessIdentifier oldProcessID, WebCore::PageIdentifier oldWebPageID, WebCore::PageIdentifier newWebPageID);
    void didCreateFrame(WebFrameProxy&);
    void willDestroyFrame(const WebFrameProxy&);
    void didCreateProvisionalFrame(ProvisionalFrameProxy&);
    void willDestroyProvisionalFrame(const ProvisionalFrameProxy&);
    void didCommitProvisionalFrame(WebFrameProxy&, WebCore::ProcessIdentifier oldProcessID, std::optional<WebCore::PageIdentifier> oldPageID, WebCore::ProcessIdentifier newProcessID);

    InspectorBrowserAgent* NODELETE enabledBrowserAgent() const;
    void setEnabledBrowserAgent(InspectorBrowserAgent*);

    void browserExtensionsEnabled(HashMap<String, String>&&);
    void browserExtensionsDisabled(HashSet<String>&&);

    bool isNetworkInstrumentationEnabled() const;
    bool isPageInstrumentationEnabled() const;

    void setShowPaintRects(bool);

private:
    WebPageAgentContext NODELETE webPageAgentContext();
    void createLazyAgents();

    void addTarget(std::unique_ptr<InspectorTargetProxy>&&);
    void removeTarget(const String& targetId);

    bool shouldManageFrameTargets() const;

    const Ref<Inspector::FrontendRouter> m_frontendRouter;
    const Ref<Inspector::BackendDispatcher> m_backendDispatcher;
    Inspector::AgentRegistry m_agents;

    WeakRef<WebPageProxy> m_inspectedPage;

    CheckedPtr<Inspector::InspectorTargetAgent> m_targetAgent;
    HashMap<String, std::unique_ptr<InspectorTargetProxy>> m_targets;

    WebPageInspectorEmulationAgent* m_emulationAgent { nullptr };
    WebPageInspectorInputAgent* m_inputAgent { nullptr };
    InspectorScreencastAgent* m_screecastAgent { nullptr };

    CheckedPtr<InspectorBrowserAgent> m_enabledBrowserAgent;
    RefPtr<Inspector::ProxyingNetworkAgent> m_networkAgent;
    RefPtr<Inspector::ProxyingPageAgent> m_pageAgent;

    bool m_didCreateLazyAgents { false };
    UncheckedKeyHashMap<WebCore::NavigationIdentifier, NavigationHandler> m_pendingNavigations;

    static WebPageInspectorControllerObserver* s_observer;
};

} // namespace WebKit
