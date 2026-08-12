#pragma once

#include <crow.h>
#include <string>

namespace treeminer {
class IFindJournal;
class SubmissionManager;
}

crow::SimpleApp& getApp();
bool isValidDashboardBind(const std::string& address);
std::string getConsoleUrl(const std::string& bind_address);
void startServer(const std::string& bind_address);
void setupRoutes(treeminer::IFindJournal* journal,
                 treeminer::SubmissionManager* submission_manager);
