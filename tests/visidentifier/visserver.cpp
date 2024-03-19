/*
 * Copyright (C) 2024 Renesas Electronics Corporation.
 * Copyright (C) 2024s EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>

#include <Poco/Format.h>
#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/SecureServerSocket.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/URI.h>

#include "logger/logger.hpp"
#include "visidentifier/vismessage.hpp"
#include "visserver.hpp"

/***********************************************************************************************************************
 * Static
 **********************************************************************************************************************/

#define LOG_DBG() LOG_MODULE_DBG(AosLogModule(LogModuleEnum::eNumModules))
#define LOG_INF() LOG_MODULE_INF(AosLogModule(LogModuleEnum::eNumModules))
#define LOG_WRN() LOG_MODULE_WRN(AosLogModule(LogModuleEnum::eNumModules))
#define LOG_ERR() LOG_MODULE_ERR(AosLogModule(LogModuleEnum::eNumModules))

static const std::string cLogPrefix = "[Test VIS Service]";

/***********************************************************************************************************************
 * VISParams
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

void VISParams::Set(const std::string& key, const std::string& value)
{
    std::lock_guard lock(mMutex);

    mMap[key] = {value};
}

void VISParams::Set(const std::string& key, const std::vector<std::string>& values)
{
    std::lock_guard lock(mMutex);

    mMap[key] = values;
}

std::vector<std::string> VISParams::Get(const std::string& key)
{
    std::lock_guard lock(mMutex);

    if (const auto it = mMap.find(key); it != mMap.end()) {
        return it->second;
    }

    throw std::runtime_error("key not found");
}

VISParams& VISParams::Instance()
{
    static VISParams instance;
    return instance;
}

/***********************************************************************************************************************
 * WebSocketRequestHandler
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

void WebSocketRequestHandler::handleRequest(
    Poco::Net::HTTPServerRequest& request, Poco::Net::HTTPServerResponse& response)
{
    try {
        Poco::Net::WebSocket ws(request, response);

        LOG_INF() << Poco::format("%s Web Socket has been connection established.", cLogPrefix).c_str();

        int                flags;
        int                n;
        Poco::Buffer<char> buffer(0);

        do {
            n = ws.receiveFrame(buffer, flags);

            if (n == 0) {
                continue;
            } else if ((flags & Poco::Net::WebSocket::FRAME_OP_BITMASK) == Poco::Net::WebSocket::FRAME_OP_CLOSE) {
                ws.sendFrame(nullptr, 0, flags);
                break;
            }

            const std::string frameStr(buffer.begin(), buffer.end());

            buffer.resize(0);

            LOG_DBG() << Poco::format(
                "%s Frame received (val=%s, length=%d, flags=0x%x).", cLogPrefix, frameStr, n, unsigned(flags))
                             .c_str();

            const auto responseFrame = handleFrame(frameStr);

            ws.sendFrame(responseFrame.c_str(), responseFrame.length(), flags);
        } while (n > 0 && (flags & Poco::Net::WebSocket::FRAME_OP_BITMASK) != Poco::Net::WebSocket::FRAME_OP_CLOSE);

        LOG_DBG() << Poco::format("%s Web Socket has been closed.", cLogPrefix).c_str();

    } catch (const Poco::Net::WebSocketException& exc) {
        LOG_ERR() << Poco::format("%s Caught exception: messag = %s.", cLogPrefix, exc.what()).c_str();

        switch (exc.code()) {
        case Poco::Net::WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
            response.set("Sec-WebSocket-Version", Poco::Net::WebSocket::WEBSOCKET_VERSION);
            // fallthrough
        case Poco::Net::WebSocket::WS_ERR_NO_HANDSHAKE:
        case Poco::Net::WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
        case Poco::Net::WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
            response.setContentLength(0);
            response.send();

            break;
        }
    }
}

/***********************************************************************************************************************
 * Private
 **********************************************************************************************************************/

std::string WebSocketRequestHandler::handleGetRequest(const VISMessage& request)
{
    VISMessage response(VISAction::EnumType::eGet, request.GetValue<std::string>(VISMessage::cRequestIdTagName), "");

    Poco::JSON::Array valueArray;

    for (const auto& value : VISParams::Instance().Get(request.GetValue<std::string>(VISMessage::cPathTagName))) {
        valueArray.add(value);
    }

    if (valueArray.size() > 1) {
        response.SetKeyValue("value", valueArray);
    } else {
        response.SetKeyValue("value", valueArray.empty() ? "" : valueArray.begin()->extract<std::string>());
    }

    return response.ToString();
}

std::string WebSocketRequestHandler::handleSubscribeRequest(const VISMessage& request)
{
    static uint32_t lastSubscribeId {0};

    const auto requestId      = request.GetValue<std::string>(VISMessage::cRequestIdTagName);
    const auto subscriptionId = std::to_string(lastSubscribeId++);

    VISMessage response(VISAction::EnumType::eSubscribe);

    response.SetKeyValue(VISMessage::cRequestIdTagName, requestId);
    response.SetKeyValue(VISMessage::cSubscriptionIdTagName, subscriptionId);

    return response.ToString();
}

std::string WebSocketRequestHandler::handleUnsubscribeAllRequest(const VISMessage& request)
{
    return request.ToString();
}

std::string WebSocketRequestHandler::handleFrame(const std::string& frame)
{
    const VISMessage request(frame);

    if (request.Is(VISActionEnum::eGet)) {
        return handleGetRequest(frame);
    } else if (request.Is(VISActionEnum::eSubscribe)) {
        return handleSubscribeRequest(frame);
    } else if (request.Is(VISActionEnum::eUnsubscribeAll)) {
        return handleUnsubscribeAllRequest(frame);
    }

    return frame;
}

/***********************************************************************************************************************
 * RequestHandlerFactory
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

Poco::Net::HTTPRequestHandler* RequestHandlerFactory::createRequestHandler(const Poco::Net::HTTPServerRequest&)
{
    return new WebSocketRequestHandler;
}

/***********************************************************************************************************************
 * VISWebSocketServer
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

VISWebSocketServer& VISWebSocketServer::Instance()
{
    static VISWebSocketServer wsServer;

    return wsServer;
}

void VISWebSocketServer::Start(const std::string& keyPath, const std::string& certPath, const std::string& uriStr)
{
    std::lock_guard lock(mMutex);

    Stop();

    mThread = std::thread(&VISWebSocketServer::RunServiceThreadF, this, keyPath, certPath, uriStr);
}

void VISWebSocketServer::Stop()
{
    std::lock_guard lock(mMutex);

    if (mThread.joinable()) {
        mStopEvent.set();

        mThread.join();
    }
}

bool VISWebSocketServer::TryWaitServiceStart(const long timeout)
{
    return mStartEvent.tryWait(timeout);
}

/***********************************************************************************************************************
 * Private
 **********************************************************************************************************************/

void VISWebSocketServer::RunServiceThreadF(
    const std::string keyPath, const std::string certPath, const std::string uriStr)
{
    try {
        Poco::SharedPtr<Poco::Net::AcceptCertificateHandler> cert = new Poco::Net::AcceptCertificateHandler(false);

        Poco::Net::Context::Ptr context = new Poco::Net::Context(Poco::Net::Context::SERVER_USE, keyPath, certPath, "",
            Poco::Net::Context::VERIFY_NONE, 9, false, "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

        Poco::Net::SSLManager::instance().initializeClient(0, cert, context);

        const auto                    port = Poco::URI(uriStr).getPort();
        Poco::Net::SecureServerSocket svs(port, 64, context);
        Poco::Net::HTTPServer         srv(new RequestHandlerFactory, svs, new Poco::Net::HTTPServerParams);

        srv.start();

        mStartEvent.set();

        mStopEvent.wait();

        srv.stop();
    } catch (const Poco::Exception& e) {
        LOG_ERR() << Poco::format("%s RunServiceThreadF caught exception: messag = %s.", cLogPrefix, e.what()).c_str();
    }
}
