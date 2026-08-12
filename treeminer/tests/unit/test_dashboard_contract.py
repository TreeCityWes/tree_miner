from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_local_dashboard_is_read_only_and_has_an_explicit_bind():
    server = (ROOT / "src" / "LocalServer.cpp").read_text()

    assert "s_app.bindaddr(bind_address)" in server
    assert 'std::string dashboardBind = "127.0.0.1"' in (ROOT / "src" / "main.cpp").read_text()
    assert 'CROW_ROUTE(s_app, "/")' in server
    assert 'CROW_ROUTE(s_app, "/api/rig")' in server
    assert 'CROW_ROUTE(s_app, "/assets/hashfield.webp")' in server
    assert "CROW_ROUTE(s_app, \"/api/rig\").methods" not in server
    assert "getConsoleUrl" in server

    main = (ROOT / "src" / "main.cpp").read_text()
    assert '("dashboard-bind", po::value<std::string>()' in main
    assert 'configuredValue("dashboard_bind")' in main
    assert "isValidDashboardBind(dashboardBind)" in main
    assert '"Local console: " + getConsoleUrl(dashboardBind)' in main
    assert 'std::thread serverThread(startServer, dashboardBind)' in main


def test_dashboard_bind_validation_uses_ip_literals_and_formats_ipv6_urls():
    server = (ROOT / "src" / "LocalServer.cpp").read_text()

    assert "inet_pton(AF_INET, address.c_str(), &ipv4)" in server
    assert "inet_pton(AF_INET6, address.c_str(), &ipv6)" in server
    assert 'bind_address == "0.0.0.0"' in server
    assert 'bind_address == "::"' in server
    assert 'ipv6 ? "[" + address + "]" : address' in server


def test_dashboard_exposes_operator_language_and_live_fields():
    page = (ROOT / "src" / "DashboardPage.h").read_text()
    reporter = (ROOT / "src" / "StatReporter.cpp").read_text()

    assert "UPSTREAM OFFLINE" in page
    assert "Mining continues" in page
    assert "Q_XNM" in page and "Q_XUNI" in page
    assert "Combined throughput" in page
    assert "Export snapshot" in page
    assert "getMinerDashboardData" in reporter
    assert 'result["delivery"]' in reporter
    assert 'result["gpus"]' in reporter


def test_terminal_presentation_mode_is_explicit_and_animated():
    main = (ROOT / "src" / "main.cpp").read_text()
    terminal = (ROOT / "src" / "TerminalUi.cpp").read_text()

    assert '("display", po::value<std::string>()' in main
    assert 'displayMode = "logs"' in main
    assert 'displayMode == "prompt"' in main
    assert 'displayMode == "terminal"' in main
    assert "matrixRow" in terminal
    assert "M-units/s" in terminal
    assert "Q_XNM" in terminal and "Q_XUNI" in terminal
    assert "\\033[?1049h" in terminal
    assert "\\033[?1049l" in terminal
