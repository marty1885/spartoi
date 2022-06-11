#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/plugins/Plugin.h>
#include "SpartanServer.hpp"
#include <memory>
#include <trantor/net/EventLoopThreadPool.h>
#include <vector>

namespace spartoi
{
class SpartanServer;

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

