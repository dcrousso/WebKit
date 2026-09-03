/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 * Copyright (C) 2015-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
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
#include "PageRuntimeAgent.h"

#include "DOMWrapperWorld.h"
#include "Document.h"
#include "FrameConsoleClient.h"
#include "FrameLoader.h"
#include "InspectorIdentifierRegistry.h"
#include "InspectorPageAgent.h"
#include "InstrumentingAgents.h"
#include "JSDOMWindowCustom.h"
#include "JSExecState.h"
#include "LocalFrame.h"
#include "Page.h"
#include "PageInspectorController.h"
#include "RuntimeAgentUtilities.h"
#include "ScriptController.h"
#include "ScriptSourceCode.h"
#include "SecurityOrigin.h"
#include "UserGestureEmulationScope.h"
#include <JavaScriptCore/InjectedScript.h>
#include <JavaScriptCore/InjectedScriptManager.h>
#include <JavaScriptCore/InspectorProtocolObjects.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

using namespace Inspector;

WTF_MAKE_TZONE_ALLOCATED_IMPL(PageRuntimeAgent);

PageRuntimeAgent::PageRuntimeAgent(PageAgentContext& context)
    : InspectorRuntimeAgent(context)
    , m_frontendDispatcher(makeUniqueRef<Inspector::RuntimeFrontendDispatcher>(context.frontendRouter))
    , m_backendDispatcher(Inspector::RuntimeBackendDispatcher::create(Ref { context.backendDispatcher }, this))
    , m_instrumentingAgents(context.instrumentingAgents)
    , m_inspectedPage(context.inspectedPage)
{
}

PageRuntimeAgent::~PageRuntimeAgent() = default;

Inspector::Protocol::ErrorStringOr<void> PageRuntimeAgent::enable()
{
    Ref agents = m_instrumentingAgents.get();
    if (agents->enabledPageRuntimeAgent() == this)
        return { };

    auto result = InspectorRuntimeAgent::enable();
    if (!result)
        return result;

    // Report initial contexts before enabling instrumentation as the reporting
    // can force creation of script state which could result in duplicate notifications.
    reportExecutionContextCreation();

    agents->setEnabledPageRuntimeAgent(this);

    return result;
}

Inspector::Protocol::ErrorStringOr<void> PageRuntimeAgent::disable()
{
    Ref { m_instrumentingAgents.get() }->setEnabledPageRuntimeAgent(nullptr);

    m_bindingNames.clear();

    return InspectorRuntimeAgent::disable();
}

void PageRuntimeAgent::frameNavigated(LocalFrame& frame)
{
    SetForScope ignoreDidClearWindowObject(m_ignoreDidClearWindowObject, true);
    // Ensure execution context is created for the frame even if it doesn't have scripts.
    mainWorldGlobalObject(frame);
}

static JSC_DECLARE_HOST_FUNCTION(bindingCallback);

JSC_DEFINE_HOST_FUNCTION(bindingCallback, (JSC::JSGlobalObject * globalObject, JSC::CallFrame* callFrame))
{
    auto result = JSC::JSValue::encode(JSC::jsUndefined());
    if (!callFrame->jsCallee())
        return result;
    String bindingName;
    if (auto* function = dynamicDowncast<JSC::JSFunction>(callFrame->jsCallee()))
        bindingName = function->name(globalObject->vm());
    auto client = globalObject->consoleClient();
    if (!client)
        return result;
    if (callFrame->argumentCount() < 1)
        return result;
    auto value = callFrame->argument(0);
    if (value.isUndefined())
        return result;
    String stringArg = value.toWTFString(globalObject);
    client->bindingCalled(globalObject, bindingName, stringArg);
    return result;
}

static void addBindingToFrame(LocalFrame& frame, const String& name)
{
    JSC::JSGlobalObject* globalObject = frame.script().globalObject(mainThreadNormalWorldSingleton());
    auto& vm = globalObject->vm();
    JSC::JSLockHolder lock(vm);
    globalObject->putDirectNativeFunction(vm, globalObject, JSC::Identifier::fromString(vm, name), 1, bindingCallback, JSC::ImplementationVisibility::Public, JSC::NoIntrinsic, JSC::attributesForStructure(static_cast<unsigned>(JSC::PropertyAttribute::Function)));
}

Inspector::Protocol::ErrorStringOr<void> PageRuntimeAgent::addBinding(const String& name)
{
    if (!m_bindingNames.add(name).isNewEntry)
        return {};

    m_inspectedPage->forEachLocalFrame([&](LocalFrame& frame) {
        if (!frame.script().canExecuteScripts(ReasonForCallingCanExecuteScripts::NotAboutToExecuteScript))
            return;

        addBindingToFrame(frame, name);
    });

    return {};
}

void PageRuntimeAgent::bindingCalled(JSC::JSGlobalObject* globalObject, const String& name, const String& arg)
{
    auto injectedScript = injectedScriptManager().injectedScriptFor(globalObject);
    if (injectedScript.hasNoValue())
        return;
    m_frontendDispatcher->bindingCalled(injectedScriptManager().injectedScriptIdFor(globalObject), name, arg);
}

void PageRuntimeAgent::didClearWindowObjectInWorld(LocalFrame& frame, DOMWrapperWorld& world)
{
    if (m_ignoreDidClearWindowObject)
        return;

    auto frameId = m_inspectedPage->inspectorController().identifierRegistry().frameId(&frame);
    if (frameId.isEmpty())
        return;

    SetForScope ignoreDidClearWindowObject(m_ignoreDidClearWindowObject, true);

    if (world.isNormal()) {
        for (const auto& name : m_bindingNames)
            addBindingToFrame(frame, name);
    }

    notifyContextCreated(frameId, frame.script().globalObject(world), world);
}

void PageRuntimeAgent::didReceiveMainResourceError(LocalFrame& frame)
{
    if (frame.loader().stateMachine().isDisplayingInitialEmptyDocument()) {
        // Ensure execution context is created for the empty docment to make
        // it usable in case loading failed.
        mainWorldGlobalObject(frame);
    }
}

InjectedScript PageRuntimeAgent::injectedScriptForEval(Inspector::Protocol::ErrorString& errorString, std::optional<Inspector::Protocol::Runtime::ExecutionContextId>&& executionContextId)
{
    if (!executionContextId) {
        RefPtr localMainFrame = m_inspectedPage->localMainFrame();
        if (!localMainFrame)
            return InjectedScript();

        auto result = injectedScriptManager().injectedScriptFor(&mainWorldGlobalObject(*localMainFrame));
        if (result.hasNoValue())
            errorString = "Internal error: main world execution context not found"_s;
        return result;
    }

    auto injectedScript = injectedScriptManager().injectedScriptForId(*executionContextId);
    if (injectedScript.hasNoValue())
        errorString = "Missing injected script for given executionContextId"_s;
    return injectedScript;
}

void PageRuntimeAgent::muteConsole()
{
    FrameConsoleClient::mute();
}

void PageRuntimeAgent::unmuteConsole()
{
    FrameConsoleClient::unmute();
}

void PageRuntimeAgent::reportExecutionContextCreation()
{
    Ref identifierRegistry = m_inspectedPage->inspectorController().identifierRegistry();

    protect(m_inspectedPage)->forEachLocalFrame([&](LocalFrame& frame) {
        auto frameId = identifierRegistry->frameId(&frame);

        // Always send the main world first.
        auto& mainGlobalObject = mainWorldGlobalObject(frame);
        notifyContextCreated(frameId, &mainGlobalObject, mainThreadNormalWorldSingleton());

        for (auto& jsWindowProxy : frame.windowProxy().jsWindowProxiesAsVector()) {
            auto* globalObject = jsWindowProxy->window();
            if (globalObject == &mainGlobalObject)
                continue;

            Ref securityOrigin = protect(downcast<LocalDOMWindow>(jsWindowProxy->wrapped()).document())->securityOrigin();
            notifyContextCreated(frameId, globalObject, jsWindowProxy->world(), securityOrigin.ptr());
        }
    });
}

void PageRuntimeAgent::notifyContextCreated(const Inspector::Protocol::Network::FrameId& frameId, JSC::JSGlobalObject* globalObject, const DOMWrapperWorld& world, SecurityOrigin* securityOrigin)
{
    auto injectedScript = injectedScriptManager().injectedScriptFor(globalObject);
    if (injectedScript.hasNoValue())
        return;

    auto name = world.name();
    if (name.isEmpty() && securityOrigin)
        name = securityOrigin->toRawString();

    m_frontendDispatcher->executionContextCreated(Inspector::Protocol::Runtime::ExecutionContextDescription::create()
        .setId(injectedScriptManager().injectedScriptIdFor(globalObject))
        .setType(toProtocol(world.type()))
        .setName(name)
        .setFrameId(frameId)
        .release());
}

Inspector::Protocol::ErrorStringOr<std::tuple<Ref<Inspector::Protocol::Runtime::RemoteObject>, std::optional<bool> /* wasThrown */, std::optional<int> /* savedResultIndex */>> PageRuntimeAgent::evaluate(const String& expression, const String& objectGroup, std::optional<bool>&& includeCommandLineAPI, std::optional<bool>&& doNotPauseOnExceptionsAndMuteConsole, std::optional<Inspector::Protocol::Runtime::ExecutionContextId>&& executionContextId, std::optional<bool>&& returnByValue, std::optional<bool>&& generatePreview, std::optional<bool>&& saveResult, std::optional<bool>&& emulateUserGesture)
{
    Inspector::Protocol::ErrorString errorString;

    auto injectedScript = injectedScriptForEval(errorString, WTF::move(executionContextId));
    if (injectedScript.hasNoValue())
        return makeUnexpected(errorString);

    UserGestureEmulationScope userGestureScope(m_inspectedPage, emulateUserGesture.value_or(false), dynamicDowncast<Document>(executionContext(injectedScript.globalObject())));
    return InspectorRuntimeAgent::evaluate(injectedScript, expression, objectGroup, WTF::move(includeCommandLineAPI), WTF::move(doNotPauseOnExceptionsAndMuteConsole), WTF::move(returnByValue), WTF::move(generatePreview), WTF::move(saveResult), WTF::move(emulateUserGesture));
}

void PageRuntimeAgent::callFunctionOn(const Inspector::Protocol::Runtime::RemoteObjectId& objectId, const String& expression, RefPtr<JSON::Array>&& optionalArguments, std::optional<bool>&& doNotPauseOnExceptionsAndMuteConsole, std::optional<bool>&& returnByValue, std::optional<bool>&& generatePreview, std::optional<bool>&& emulateUserGesture, std::optional<bool>&& awaitPromise, Ref<CallFunctionOnCallback>&& callback)
{
    auto injectedScript = injectedScriptManager().injectedScriptForObjectId(objectId);
    if (injectedScript.hasNoValue()) {
        callback->sendFailure("Missing injected script for given objectId"_s);
        return;
    }

    UserGestureEmulationScope userGestureScope(m_inspectedPage, emulateUserGesture.value_or(false), dynamicDowncast<Document>(executionContext(injectedScript.globalObject())));
    return InspectorRuntimeAgent::callFunctionOn(objectId, expression, WTF::move(optionalArguments), WTF::move(doNotPauseOnExceptionsAndMuteConsole), WTF::move(returnByValue), WTF::move(generatePreview), WTF::move(emulateUserGesture), WTF::move(awaitPromise), WTF::move(callback));
}

} // namespace WebCore
