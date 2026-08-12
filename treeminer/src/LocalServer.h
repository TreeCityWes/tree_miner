#pragma once

#include <crow.h>
#include <string>

namespace treeminer {
class IFindJournal;
class SubmissionManager;
}

crow::SimpleApp& getApp();
std::string getConsoleUrl();
void startServer();
void setupRoutes(treeminer::IFindJournal* journal,
                 treeminer::SubmissionManager* submission_manager);
