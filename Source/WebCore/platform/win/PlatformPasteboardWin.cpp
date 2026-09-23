/*
 * Copyright (C) 2026 Microsoft Corporation. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PlatformPasteboard.h"

#if PLATFORM(WIN)

#include "ClipboardUtilitiesWin.h"
#include "CommonAtomStrings.h"
#include "PasteboardCustomData.h"
#include "PasteboardItemInfo.h"
#include "SharedBuffer.h"
#include "WebCoreInstanceHandle.h"
#include <windows.h>
#include <wtf/HashMap.h>
#include <wtf/ListHashSet.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

// `EmptyClipboard` makes the owner of an unowned clipboard `nullptr`, which then makes `SetClipboardData` fail.
static HWND clipboardOwner()
{
    ASSERT(isMainThread());

    static HWND owner = []() -> HWND {
        WNDCLASS windowClass { };
        windowClass.lpfnWndProc = ::DefWindowProc;
        windowClass.hInstance = WebCore::instanceHandle();
        windowClass.lpszClassName = L"PlatformPasteboardOwnerWindowClass";
        if (!::RegisterClass(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        return ::CreateWindow(windowClass.lpszClassName, L"PlatformPasteboardOwnerWindow", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, windowClass.hInstance, 0);
    }();
    return owner;
}

static String readClipboardString(UINT format)
{
    if (!::IsClipboardFormatAvailable(format) || !::OpenClipboard(clipboardOwner()))
        return { };

    String string;
    if (HANDLE data = ::GetClipboardData(format)) {
        if (auto* characters = ::GlobalLock(data)) {
            if (format == CF_TEXT) {
                // FIXME: This treats the characters as Latin-1, not UTF-8 or even Windows Latin-1. Is that the right encoding?
                string = String::fromLatin1(static_cast<const char*>(characters));
            } else
                string = static_cast<const wchar_t*>(characters);
            ::GlobalUnlock(data);
        }
    }
    ::CloseClipboard();
    return string;
}

// The caller is responsible for having opened the clipboard.
static void setClipboardData(UINT format, HGLOBAL data)
{
    if (!data)
        return;
    if (!::SetClipboardData(format, data))
        ::GlobalFree(data);
}

static String plainText(const PasteboardCustomData& data)
{
    String text;
    data.forEachPlatformStringOrBuffer([&](auto& type, auto& value) {
        if (type == textPlainContentTypeAtom() && std::holds_alternative<String>(value))
            text = std::get<String>(value);
    });
    return text;
}

class PlatformPasteboardBackend {
public:
    virtual ~PlatformPasteboardBackend() = default;

    virtual int64_t changeCount() const = 0;
    virtual void getTypes(Vector<String>&) const = 0;
    virtual std::optional<PasteboardCustomData> readCustomData() const = 0;
    virtual String readString(size_t index, const String& type) const = 0;
    virtual int64_t write(const PasteboardCustomData&) = 0;
};

class SystemPlatformPasteboard final : public PlatformPasteboardBackend {
public:
    int64_t changeCount() const final
    {
        return ::GetClipboardSequenceNumber();
    }

    void getTypes(Vector<String>& types) const final
    {
        if (::IsClipboardFormatAvailable(CF_UNICODETEXT) || ::IsClipboardFormatAvailable(CF_TEXT))
            types.append(textPlainContentTypeAtom());
    }

    std::optional<PasteboardCustomData> readCustomData() const final
    {
        if (!::IsClipboardFormatAvailable(customDataClipboardFormat()) || !::OpenClipboard(clipboardOwner()))
            return std::nullopt;

        std::optional<PasteboardCustomData> customData;
        if (HANDLE data = ::GetClipboardData(customDataClipboardFormat())) {
            // Custom data is a serialized buffer rather than text, so it is read by length.
            auto size = ::GlobalSize(data);
            if (auto* bytes = static_cast<const uint8_t*>(::GlobalLock(data))) {
                customData = PasteboardCustomData::fromPersistenceDecoder({ { bytes, size } });
                ::GlobalUnlock(data);
            }
        }
        ::CloseClipboard();
        return customData;
    }

    String readString(size_t index, const String& type) const final
    {
        // The clipboard holds a single item.
        if (index || !type.startsWith(textPlainContentTypeAtom()))
            return { };

        auto text = readClipboardString(CF_UNICODETEXT);
        return text.isNull() ? readClipboardString(CF_TEXT) : text;
    }

    int64_t write(const PasteboardCustomData& data) final
    {
        auto text = plainText(data);
        if (text.isNull() || !::OpenClipboard(clipboardOwner()))
            return changeCount();

        if (!::EmptyClipboard()) {
            ::CloseClipboard();
            return changeCount();
        }

        replaceNewlinesWithWindowsStyleNewlines(text);
        setClipboardData(CF_UNICODETEXT, createGlobalData(text));

        if (data.hasSameOriginCustomData() || !data.origin().isEmpty())
            setClipboardData(customDataClipboardFormat(), createGlobalData(data.createSharedBuffer()->span()));

        ::CloseClipboard();
        return changeCount();
    }
};

class IsolatedPlatformPasteboard final : public PlatformPasteboardBackend {
public:
    int64_t changeCount() const final
    {
        return m_changeCount;
    }

    void getTypes(Vector<String>& types) const final
    {
        if (m_pasteboard.contains(textPlainContentTypeAtom()))
            types.append(textPlainContentTypeAtom());
    }

    std::optional<PasteboardCustomData> readCustomData() const final
    {
        return m_customData;
    }

    String readString(size_t index, const String& type) const final
    {
        if (index || !type.startsWith(textPlainContentTypeAtom()))
            return { };
        return m_pasteboard.get(textPlainContentTypeAtom());
    }

    int64_t write(const PasteboardCustomData& data) final
    {
        auto text = plainText(data);
        if (text.isNull())
            return changeCount();

        m_pasteboard.clear();
        m_pasteboard.set(textPlainContentTypeAtom(), text);
        m_customData = data.hasSameOriginCustomData() || !data.origin().isEmpty() ? std::optional { data } : std::nullopt;
        return ++m_changeCount;
    }

private:
    // This is intentionally process wide: one browser shares one clipboard across all of its contexts and pages.
    UncheckedKeyHashMap<String, String> m_pasteboard;
    std::optional<PasteboardCustomData> m_customData;
    int64_t m_changeCount { 0 };
};

static PlatformPasteboardBackend& systemPlatformPasteboard()
{
    static NeverDestroyed<SystemPlatformPasteboard> pasteboard;
    return pasteboard.get();
}

static PlatformPasteboardBackend& isolatedPlatformPasteboard()
{
    static NeverDestroyed<IsolatedPlatformPasteboard> pasteboard;
    return pasteboard.get();
}

static PlatformPasteboardBackend*& selectedPlatformPasteboard()
{
    static PlatformPasteboardBackend* pasteboard = &systemPlatformPasteboard();
    return pasteboard;
}

void PlatformPasteboard::setIsolated(bool isolated)
{
    selectedPlatformPasteboard() = isolated ? &isolatedPlatformPasteboard() : &systemPlatformPasteboard();
}

PlatformPasteboard::PlatformPasteboard(const String&)
    : m_backend(selectedPlatformPasteboard())
{
}

void PlatformPasteboard::performAsDataOwner(DataOwnerType, NOESCAPE Function<void()>&& actions)
{
    actions();
}

int64_t PlatformPasteboard::changeCount() const
{
    return m_backend->changeCount();
}

void PlatformPasteboard::getTypes(Vector<String>& types) const
{
    m_backend->getTypes(types);
}

std::optional<PasteboardCustomData> PlatformPasteboard::readCustomData() const
{
    return m_backend->readCustomData();
}

String PlatformPasteboard::readString(size_t index, const String& type) const
{
    return m_backend->readString(index, type);
}

Vector<String> PlatformPasteboard::typesSafeForDOMToReadAndWrite(const String& origin) const
{
    ListHashSet<String> domTypes;

    if (auto customData = readCustomData(); customData && customData->origin() == origin) {
        for (auto& type : customData->orderedTypes())
            domTypes.add(type);
    }

    Vector<String> types;
    getTypes(types);
    for (auto& type : types)
        domTypes.add(type);

    return copyToVector(domTypes);
}

std::optional<PasteboardItemInfo> PlatformPasteboard::informationForItemAtIndex(size_t index, int64_t changeCount)
{
    // The clipboard holds a single item, and `std::nullopt` means that it changed while it was being read.
    if (index || changeCount != this->changeCount())
        return std::nullopt;

    PasteboardItemInfo info;
    getTypes(info.platformTypesByFidelity);
    info.webSafeTypesByFidelity = info.platformTypesByFidelity;
    return info;
}

std::optional<Vector<PasteboardItemInfo>> PlatformPasteboard::allPasteboardItemInfo(int64_t changeCount)
{
    auto info = informationForItemAtIndex(0, changeCount);
    if (!info)
        return std::nullopt;

    if (info->platformTypesByFidelity.isEmpty())
        return Vector<PasteboardItemInfo> { };

    return Vector<PasteboardItemInfo> { WTF::move(*info) };
}

int PlatformPasteboard::count() const
{
    Vector<String> types;
    getTypes(types);
    return types.isEmpty() ? 0 : 1;
}

int64_t PlatformPasteboard::write(const PasteboardCustomData& data, PasteboardDataLifetime)
{
    return m_backend->write(data);
}

int64_t PlatformPasteboard::write(const Vector<PasteboardCustomData>& data, PasteboardDataLifetime pasteboardDataLifetime)
{
    // More than one custom item in the clipboard is not supported.
    if (data.isEmpty() || data.size() > 1)
        return changeCount();

    return write(data[0], pasteboardDataLifetime);
}

} // namespace WebCore

#endif // PLATFORM(WIN)
