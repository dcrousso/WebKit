/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
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

#pragma once

#include <WebCore/ProcessIdentifier.h>
#include <pal/SessionID.h>
#include <wtf/Forward.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakListHashSet.h>
#include <wtf/WeakPtr.h>

namespace WebKit {

class SuspendedPageProxy;
class WebBackForwardCacheEntry;
class WebBackForwardListItem;
class WebPageProxy;
class WebProcessPool;
class WebProcessProxy;

class WebBackForwardCache final : public CanMakeWeakPtr<WebBackForwardCache> {
    WTF_MAKE_TZONE_ALLOCATED(WebBackForwardCache);
public:
    explicit WebBackForwardCache(WebProcessPool&);
    ~WebBackForwardCache();

    void ref() const;
    void deref() const;

    void setCapacity(WebProcessPool&, unsigned);
    unsigned capacity() const { return m_capacity; }
    unsigned size() const { return m_itemsWithCachedPage.computeSize(); }

    void clear();
    void pruneToSize(unsigned);
    void removeEntriesForProcess(WebProcessProxy&);
    void removeEntriesForPage(WebPageProxy&);
    void removeEntriesForPageAndProcess(WebPageProxy&, WebProcessProxy&);
    void removeEntriesForSession(PAL::SessionID);

    void addEntry(WebBackForwardListItem&, Ref<SuspendedPageProxy>&&);
    void addEntry(WebBackForwardListItem&, WebCore::ProcessIdentifier);
    void removeEntry(WebBackForwardListItem&);
    void removeEntry(SuspendedPageProxy&);
    Ref<SuspendedPageProxy> takeSuspendedPage(WebBackForwardListItem&);

private:
    Ref<WebProcessPool> protectedProcessPool() const;

    void removeOldestEntry();
    void removeEntriesMatching(NOESCAPE const Function<bool(WebBackForwardListItem&)>&);
    void addEntry(WebBackForwardListItem&, Ref<WebBackForwardCacheEntry>&&);

    WeakRef<WebProcessPool> m_processPool;
    unsigned m_capacity { 0 };
    WeakListHashSet<WebBackForwardListItem> m_itemsWithCachedPage;
};

} // namespace WebKit
