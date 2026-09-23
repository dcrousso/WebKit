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
#include "RemoteInspectorPipe.h"

#if ENABLE(REMOTE_INSPECTOR)

#include "InspectorPlaywrightAgent.h"
#include <JavaScriptCore/InspectorFrontendChannel.h>
#include <array>
#include <wtf/Compiler.h>
#include <wtf/MainThread.h>
#include <wtf/RunLoop.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/WorkQueue.h>

#if OS(UNIX)
#include <stdio.h>
#include <unistd.h>
#endif

#if PLATFORM(WIN)
#include <io.h>
#endif

namespace WebKit {

namespace {

const int readFD = 3;
const int writeFD = 4;

constexpr size_t writePacketSize = 1 << 16;
constexpr std::array<char, 1> messageTerminator { '\0' };

#if PLATFORM(WIN)
HANDLE readHandle;
HANDLE writeHandle;
#endif

size_t readBytes(std::span<char> buffer)
{
#if PLATFORM(WIN)
    DWORD sizeRead = 0;
    if (!ReadFile(readHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &sizeRead, nullptr))
        return 0;
    return sizeRead;
#else
    while (true) {
        int sizeRead = read(readFD, buffer.data(), buffer.size());
        if (sizeRead < 0 && errno == EINTR)
            continue;
        return sizeRead > 0 ? static_cast<size_t>(sizeRead) : 0;
    }
#endif
}

void writeBytes(std::span<const char> bytes)
{
    while (!bytes.empty()) {
        auto chunk = bytes.first(std::min(bytes.size(), writePacketSize));
#if PLATFORM(WIN)
        DWORD bytesWritten = 0;
        if (!WriteFile(writeHandle, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesWritten, nullptr))
            return;
#else
        int bytesWritten = write(writeFD, chunk.data(), chunk.size());
        if (bytesWritten < 0 && errno == EINTR)
            continue;
        if (bytesWritten <= 0)
            return;
#endif
        bytes = bytes.subspan(bytesWritten);
    }
}

}  // namespace

class RemoteInspectorPipe::RemoteFrontendChannel : public Inspector::FrontendChannel {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(RemoteInspectorPipe::RemoteFrontendChannel);
public:
    RemoteFrontendChannel()
        : m_senderQueue(WorkQueue::create("Inspector pipe writer"_s))
    {
    }

    ~RemoteFrontendChannel() override = default;

    ConnectionType connectionType() const override
    {
        return ConnectionType::Remote;
    }

    void sendMessageToFrontend(const String& message) override
    {
        m_senderQueue->dispatch([message = message.isolatedCopy()]() {
            auto utf8 = message.utf8();
            writeBytes(byteCast<char>(utf8.span()));
            writeBytes(messageTerminator);
        });
    }

private:
    Ref<WorkQueue> m_senderQueue;
};

RemoteInspectorPipe::RemoteInspectorPipe(InspectorPlaywrightAgent& playwrightAgent)
    : m_playwrightAgent(playwrightAgent)
{
    m_remoteFrontendChannel = makeUnique<RemoteFrontendChannel>();
    start();
}

RemoteInspectorPipe::~RemoteInspectorPipe()
{
    stop();
}

bool RemoteInspectorPipe::start()
{
    if (m_receiverThread)
        return true;

#if PLATFORM(WIN)
    readHandle = reinterpret_cast<HANDLE>(_get_osfhandle(readFD));
    writeHandle = reinterpret_cast<HANDLE>(_get_osfhandle(writeFD));
#endif

    m_playwrightAgent.connectFrontend(*m_remoteFrontendChannel);
    m_terminated = false;
    m_receiverThread = Thread::create("Inspector pipe reader"_s, [this] {
        workerRun();
    });
    return true;
}

void RemoteInspectorPipe::stop()
{
    if (!m_receiverThread)
        return;

    m_playwrightAgent.disconnectFrontend();

    m_terminated = true;
    m_receiverThread->waitForCompletion();
    m_receiverThread = nullptr;
}

void RemoteInspectorPipe::workerRun()
{
    Vector<char> buffer(256 * 1024);
    Vector<char> line;
    while (!m_terminated) {
        size_t size = readBytes(buffer.mutableSpan());
        if (!size) {
            RunLoop::mainSingleton().dispatch([this] {
                if (!m_terminated)
                    m_playwrightAgent.disconnectFrontend();
            });
            break;
        }
        size_t start = 0;
        size_t end = line.size();
        line.append(buffer.span().first(size));
        while (true) {
            for (; end < line.size(); ++end) {
                if (line[end] == '\0')
                    break;
            }
            if (end == line.size())
                break;

            if (end > start) {
                String message = String::fromUTF8(line.span().subspan(start, end - start));
                RunLoop::mainSingleton().dispatch([this, message = WTF::move(message)] {
                    if (!m_terminated)
                        m_playwrightAgent.dispatchMessageFromFrontend(message);
                });
            }
            ++end;
            start = end;
        }
        if (start != 0 && start < line.size())
            memmoveSpan(line.mutableSpan(), line.mutableSpan().subspan(start));
        line.shrink(line.size() - start);
    }
}

} // namespace WebKit

#endif // ENABLE(REMOTE_INSPECTOR)

