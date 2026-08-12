#pragma once

#include <crow.h>
#include <string>
#include <vector>

namespace treeminer {
class IFindJournal;
class SubmissionManager;
}

crow::SimpleApp& getApp();
bool isValidDashboardBind(const std::string& address);
bool isLoopbackDashboardBind(const std::string& address);
// Bind may be 0.0.0.0 / :: (listen everywhere). These are the addresses an operator
// can actually type into a browser — never the wildcard itself.
std::vector<std::string> dashboardAdvertisedAddresses(const std::string& bind_address);
std::string formatDashboardUrl(const std::string& host);
std::string getConsoleUrl(const std::string& bind_address);
std::string dashboardReadyMessage(const std::string& bind_address);
void startServer(const std::string& bind_address);
void setupRoutes(treeminer::IFindJournal* journal,
                 treeminer::SubmissionManager* submission_manager);
