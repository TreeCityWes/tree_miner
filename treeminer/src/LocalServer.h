#pragma once

#include <crow.h>

namespace treeminer {
class IFindJournal;
class SubmissionManager;
}

crow::SimpleApp& getApp();
void startServer();
void setupRoutes(treeminer::IFindJournal* journal,
                 treeminer::SubmissionManager* submission_manager);
