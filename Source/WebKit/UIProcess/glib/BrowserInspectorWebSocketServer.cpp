/*
 * Copyright (C) 2026 Microsoft Corporation.
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
#include "BrowserInspectorWebSocketServer.h"

#if ENABLE(REMOTE_INSPECTOR)

#include "InspectorPlaywrightAgent.h"
#include "InspectorPlaywrightAgentClient.h"
#include "WebKit2Initialize.h"
#include <glib-object.h>
#include <libsoup/soup-websocket-connection.h>
#include <libsoup/soup.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/NeverDestroyed.h>

namespace WebKit {

namespace {

class WebSocketFrontendChannel : public Inspector::FrontendChannel {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(WebSocketFrontendChannel);
public:
    explicit WebSocketFrontendChannel(SoupWebsocketConnection* connection)
        : m_connection(connection)
    {
    }

    ~WebSocketFrontendChannel() override = default;

private:
   ConnectionType connectionType() const override
    {
        return ConnectionType::Remote;
    }

    void sendMessageToFrontend(const String& message) override
    {
        soup_websocket_connection_send_text(m_connection.get(), message.utf8().data());
    }

    GRefPtr<SoupWebsocketConnection> m_connection;
};

class BrowserInspectorWebSocketServer {
public:
    BrowserInspectorWebSocketServer(std::unique_ptr<InspectorPlaywrightAgentClient> client, GRefPtr<SoupServer> soupServer)
        : m_playwrightAgent(std::move(client))
        , m_soupServer(std::move(soupServer))
    {
        soup_server_add_websocket_handler(m_soupServer.get(), nullptr, nullptr, nullptr, &BrowserInspectorWebSocketServer::handleWebSocketConnection, this, nullptr);
    }

private:
    static void handleWebSocketConnection(SoupServer*, SoupServerMessage*, const char* path, SoupWebsocketConnection* connection, gpointer userData)
    {
        auto server = static_cast<BrowserInspectorWebSocketServer*>(userData);
        if (!server->handleWebSocketConnection(connection)) {
            soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "WebSocket connection already established");
            return;
        }

        g_signal_connect(connection, "closed", G_CALLBACK(+[](SoupWebsocketConnection* connection, gpointer userData) {
            auto server = static_cast<BrowserInspectorWebSocketServer*>(userData);
            server->handleConnectionClosed(connection);
        }), server);

        g_signal_connect(connection, "message", G_CALLBACK(+[] (SoupWebsocketConnection*, SoupWebsocketDataType messageType, GBytes* message, gpointer userData) {
            auto server = static_cast<BrowserInspectorWebSocketServer*>(userData);
            server->handleWebSocketMessage(messageType, message);
        }), server);
    }


    void handleConnectionClosed(SoupWebsocketConnection* connection)
    {
        if (m_frontendChannel) {
            m_playwrightAgent.disconnectFrontend();
            m_frontendChannel.reset();
        }
    }

    void handleWebSocketMessage(SoupWebsocketDataType messageType, GBytes* message)
    {
        if (messageType != SOUP_WEBSOCKET_DATA_TEXT) {
            fprintf(stderr, "WebSocket message received non-text message: %d\n", messageType);
            return;
        }

        gsize messageSize;
        gconstpointer messageData = g_bytes_get_data(message, &messageSize);
        String messageString = String::fromUTF8(std::span<const char8_t>(static_cast<const char8_t*>(messageData), messageSize));
        m_playwrightAgent.dispatchMessageFromFrontend(messageString);
    }

    bool handleWebSocketConnection(SoupWebsocketConnection* connection)
    {
        if (m_frontendChannel) {
            fprintf(stderr, "WebSocket connection already established\n");
            return false;
        }
        m_frontendChannel = std::make_unique<WebSocketFrontendChannel>(connection);
        m_playwrightAgent.connectFrontend(*m_frontendChannel);
        return true;
    }

    InspectorPlaywrightAgent m_playwrightAgent;
    GRefPtr<SoupServer> m_soupServer;
    std::unique_ptr<WebSocketFrontendChannel> m_frontendChannel;
};

} // namespace

void initializeBrowserInspectorWebSocket(unsigned port, std::unique_ptr<InspectorPlaywrightAgentClient> client)
{
    // Initialize main loop before creating inspector agent and starting the server.
    WebKit::InitializeWebKit2();

    GRefPtr<SoupServer> soupServer = adoptGRef(soup_server_new("server-header", "WebKit-Playwright-WSS", nullptr));

    GUniqueOutPtr<GError> error;
    const SoupServerListenOptions options = static_cast<SoupServerListenOptions>(0);
    if (!soup_server_listen_local(soupServer.get(), port, options, &error.outPtr())) {
        fprintf(stderr, "Failed to start WebSocket server at port %u: %s\n", port, error->message);
        return;
    }

    int actualPort = port;
    if (!actualPort) {
        GSList* uris = soup_server_get_uris(soupServer.get());
        g_assert_nonnull(uris);
        GUri* uri = static_cast<GUri*>(uris->data);
        actualPort = g_uri_get_port(uri);
        g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
    }
    if (actualPort == -1) {
        fprintf(stderr, "Failed to start WebSocket server\n");
        return;
    }

    static NeverDestroyed<BrowserInspectorWebSocketServer> server(std::move(client), std::move(soupServer));

    fprintf(stderr, "Playwright listening on ws://localhost:%d\n", actualPort);
}

} // namespace WebKit

#endif // ENABLE(REMOTE_INSPECTOR)
