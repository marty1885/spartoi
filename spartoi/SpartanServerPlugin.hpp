#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/plugins/Plugin.h>
#include "SpartanServer.hpp"
#include <functional>
#include <memory>
#include <trantor/net/EventLoopThreadPool.h>
#include <vector>

namespace spartoi
{
class SpartanServer;

using AccidentalHttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;
using AccidentalHttpHandler = std::function<void(drogon::HttpRequestPtr&&,
                                                  AccidentalHttpCallback&&)>;

// Register before drogon::app().run(). This handler is independent of Drogon's
// HTTP routes and is only called for HTTP/1.x sent to a Spartan listener.
void registerAccidentalHttpHandler(AccidentalHttpHandler handler);

class SpartanServerPlugin : public drogon::Plugin<SpartanServerPlugin>
{
public:
    SpartanServerPlugin() {}
    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

protected:
    std::shared_ptr<trantor::EventLoopThreadPool> pool_;
    std::vector<std::unique_ptr<SpartanServer>> servers_;
};
}
