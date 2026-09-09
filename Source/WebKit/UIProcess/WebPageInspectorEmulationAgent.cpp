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

#include "APIPageConfiguration.h"
#include "WebPageProxy.h"
#include "WebPreferences.h"
#include "PageClient.h"
#include <JavaScriptCore/InspectorFrontendRouter.h>
#include <WebCore/Credential.h>


namespace WebKit {

using namespace Inspector;

WTF_MAKE_TZONE_ALLOCATED_IMPL(WebPageInspectorEmulationAgent);

WebPageInspectorEmulationAgent::WebPageInspectorEmulationAgent(BackendDispatcher& backendDispatcher, WebPageProxy& page)
    : InspectorAgentBase("Emulation"_s)
    , m_backendDispatcher(EmulationBackendDispatcher::create(backendDispatcher, this))
    , m_page(page)
{
}

WebPageInspectorEmulationAgent::~WebPageInspectorEmulationAgent()
{
}

void WebPageInspectorEmulationAgent::didCreateFrontendAndBackend()
{
}

void WebPageInspectorEmulationAgent::willDestroyFrontendAndBackend(DisconnectReason)
{
   m_commandsToRunWhenShown.clear();
}

void WebPageInspectorEmulationAgent::setDeviceMetricsOverride(int width, int height, bool fixedlayout, std::optional<double>&& deviceScaleFactor, Ref<SetDeviceMetricsOverrideCallback>&& callback)
{
#if PLATFORM(GTK)
    // On gtk, fixed layout doesn't work with compositing enabled
    // FIXME: This turns off compositing forever, even if fixedLayout is disabled.
    if (fixedlayout) {
        auto copy = m_page.preferences().copy();
        copy->setAcceleratedCompositingEnabled(false);
        m_page.setPreferences(copy);
    }
#endif

    if (deviceScaleFactor)
        m_page.setCustomDeviceScaleFactor(deviceScaleFactor.value(), [] { });
    m_page.setUseFixedLayout(fixedlayout);
    if (!m_page.pageClient()->isActiveViewVisible() && m_page.configuration().relatedPage()) {
        m_commandsToRunWhenShown.append([this, width, height, callback = WTF::move(callback)]() mutable {
            setSize(width, height, WTF::move(callback));
        });
    } else {
        setSize(width, height, WTF::move(callback));
    }
}

void WebPageInspectorEmulationAgent::setSize(int width, int height, Ref<SetDeviceMetricsOverrideCallback>&& callback)
{
    platformSetSize(width, height, [callback = WTF::move(callback)](const String& error) {
        if (error.isEmpty())
            callback->sendSuccess();
        else
            callback->sendFailure(error);
    });
}

Inspector::Protocol::ErrorStringOr<void> WebPageInspectorEmulationAgent::setJavaScriptEnabled(bool enabled)
{
    auto copy = m_page.preferences().copy();
    copy->setJavaScriptEnabled(enabled);
    m_page.setPreferences(copy);
    return { };
}

Inspector::Protocol::ErrorStringOr<void> WebPageInspectorEmulationAgent::setAuthCredentials(RefPtr<JSON::Array>&& credentials)
{
    if (!credentials) {
        m_page.setAuthCredentialsForAutomation(std::nullopt);
        return { };
    }

    Vector<std::pair<WebCore::Credential, std::optional<URL>>> parsedCredentials;
    for (unsigned i = 0; i < credentials->length(); ++i) {
        RefPtr<JSON::Object> object = credentials->get(i)->asObject();
        if (!object)
            return makeUnexpected("Credentials must be an object"_s);

        String username;
        if (!object->getString("username"_s, username))
            return makeUnexpected("Credentials must contain username"_s);

        String password;
        if (!object->getString("password"_s, password))
            return makeUnexpected("Credentials must contain password"_s);

        String origin = object->getString("origin"_s);
        std::optional<URL> originURL;
        if (!origin.isEmpty())
            originURL = URL(origin);
        parsedCredentials.append({ WebCore::Credential(username, password, WebCore::CredentialPersistence::Permanent), WTF::move(originURL) });
    }
    m_page.setAuthCredentialsForAutomation(WTF::move(parsedCredentials));
    return { };
}

Inspector::Protocol::ErrorStringOr<void> WebPageInspectorEmulationAgent::setActiveAndFocused(std::optional<bool>&& active)
{
    m_page.setActiveForAutomation(WTF::move(active));
    return { };
}

Inspector::Protocol::ErrorStringOr<void> WebPageInspectorEmulationAgent::grantPermissions(const String& origin, Ref<JSON::Array>&& values)
{
    HashSet<String> set;
    for (const auto& value : values.get()) {
        String name;
        if (!value->asString(name))
            return makeUnexpected("Permission must be a string"_s);

        set.add(name);
    }
    m_permissions.set(origin, WTF::move(set));
    m_page.setPermissionsForAutomation(m_permissions);
    return { };
}

Inspector::Protocol::ErrorStringOr<void> WebPageInspectorEmulationAgent::resetPermissions()
{
    m_permissions.clear();
    m_page.setPermissionsForAutomation(m_permissions);
    return { };
}

Inspector::Protocol::ErrorStringOr<void> WebPageInspectorEmulationAgent::setOrientationOverride(std::optional<int>&& angle)
{
#if ENABLE(ORIENTATION_EVENTS)
    m_page.setOrientationOverride(WTF::move(angle));
    return { };
#else
    UNUSED_PARAM(angle);
    return makeUnexpected("Orientation events are disabled in this build"_s);
#endif
}


void WebPageInspectorEmulationAgent::didShowPage()
{
    for (auto& command : m_commandsToRunWhenShown)
        command();
    m_commandsToRunWhenShown.clear();
}

} // namespace WebKit
