#pragma once

#include <crow.h>
#include <string>

crow::SimpleApp& getApp();
std::string getConsoleUrl();
void startServer();
void setupRoutes();
