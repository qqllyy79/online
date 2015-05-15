/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Parts of this file is covered by:

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

 */

#include "config.h"

// This is the main source for the loolwsd program. LOOL uses several loolwsd processes: one main
// parent process that listens on the TCP port and accepts connections from LOOL clients, and a
// number of child processes, each which handles a viewing (editing) session for one document.

#include <errno.h>
#include <unistd.h>

#ifdef __linux
#include <pwd.h>
#include <sys/capability.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitInit.h>

#include <Poco/Exception.h>
#include <Poco/File.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Path.h>
#include <Poco/Process.h>
#include <Poco/Util/HelpFormatter.h>
#include <Poco/Util/Option.h>
#include <Poco/Util/OptionException.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Util/ServerApplication.h>

#include "LOOLProtocol.hpp"
#include "LOOLSession.hpp"
#include "LOOLWSD.hpp"
#include "Util.hpp"

using namespace LOOLProtocol;

using Poco::File;
using Poco::Net::HTTPClientSession;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPRequestHandlerFactory;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServer;
using Poco::Net::HTTPServerParams;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServerResponse;
using Poco::Net::ServerSocket;
using Poco::Net::SocketAddress;
using Poco::Net::WebSocket;
using Poco::Net::WebSocketException;
using Poco::Path;
using Poco::Process;
using Poco::Runnable;
using Poco::Thread;
using Poco::Util::Application;
using Poco::Util::HelpFormatter;
using Poco::Util::IncompatibleOptionsException;
using Poco::Util::MissingOptionException;
using Poco::Util::Option;
using Poco::Util::OptionSet;
using Poco::Util::ServerApplication;

#if ENABLE_DEBUG
uid_t uid = 0;
#endif

class WebSocketRequestHandler: public HTTPRequestHandler
    /// Handle a WebSocket connection.
{
public:
    WebSocketRequestHandler()
    {
    }

    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override
    {
        if(!(request.find("Upgrade") != request.end() && Poco::icompare(request["Upgrade"], "websocket") == 0))
        {
            response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
            response.setContentLength(0);
            response.send();
            return;
        }

        Application& app = Application::instance();
        try
        {
            try
            {
                WebSocket ws(request, response);

                std::shared_ptr<MasterProcessSession> session;

                if (request.getURI() == LOOLWSD::CHILD_URI && request.serverAddress().port() == LOOLWSD::MASTER_PORT_NUMBER)
                {
                    session.reset(new MasterProcessSession(ws, LOOLSession::Kind::ToPrisoner));
                }
                else
                {
                    session.reset(new MasterProcessSession(ws, LOOLSession::Kind::ToClient));
                }

                // Loop, receiving WebSocket messages either from the
                // client, or from the child process (to be forwarded to
                // the client).
                int flags;
                int n;
                ws.setReceiveTimeout(0);
                do
                {
                    char buffer[100000];
                    n = ws.receiveFrame(buffer, sizeof(buffer), flags);

                    if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                        if (!session->handleInput(buffer, n))
                            n = 0;
                }
                while (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
            }
            catch (WebSocketException& exc)
            {
                app.logger().error(Util::logPrefix() + "WebSocketException: " + exc.message());
                switch (exc.code())
                {
                case WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
                    response.set("Sec-WebSocket-Version", WebSocket::WEBSOCKET_VERSION);
                    // fallthrough
                case WebSocket::WS_ERR_NO_HANDSHAKE:
                case WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
                case WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
                    response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
                    response.setContentLength(0);
                    response.send();
                    break;
                }
            }
        }
        catch (Poco::IOException& exc)
        {
            app.logger().error(Util::logPrefix() + "IOException: " + exc.message());
        }
    }
};

class RequestHandlerFactory: public HTTPRequestHandlerFactory
{
public:
    RequestHandlerFactory()
    {
    }

    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest& request) override
    {
        Application& app = Application::instance();
        std::string line = (Util::logPrefix() + "Request from " +
                            request.clientAddress().toString() + ": " +
                            request.getMethod() + " " +
                            request.getURI() + " " +
                            request.getVersion());

        for (HTTPServerRequest::ConstIterator it = request.begin(); it != request.end(); ++it)
        {
            line += " / " + it->first + ": " + it->second;
        }

        app.logger().information(line);
        return new WebSocketRequestHandler();
    }
};

class TestOutput: public Runnable
{
public:
    TestOutput(WebSocket& ws) :
        _ws(ws)
    {
    }

    void run() override
    {
        int flags;
        int n;
        Application& app = Application::instance();
        _ws.setReceiveTimeout(0);
        try
        {
            do
            {
                char buffer[100000];
                n = _ws.receiveFrame(buffer, sizeof(buffer), flags);

                if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                {
                    std::cout <<
                        Util::logPrefix() <<
                        "Client got " << n << " bytes: " << getAbbreviatedMessage(buffer, n) <<
                        std::endl;
                }
            }
            while (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
        }
        catch (WebSocketException& exc)
        {
            app.logger().error(Util::logPrefix() + "WebSocketException: " + exc.message());
            _ws.close();
        }
    }

private:
    WebSocket& _ws;
};

class TestInput: public Runnable
{
public:
    TestInput(ServerApplication& main, ServerSocket& svs, HTTPServer& srv) :
        _main(main),
        _svs(svs),
        _srv(srv)
    {
    }

    void run() override
    {
        HTTPClientSession cs("127.0.0.1", _svs.address().port());
        HTTPRequest request(HTTPRequest::HTTP_GET, "/ws");
        HTTPResponse response;
        WebSocket ws(cs, request, response);

        Thread thread;
        TestOutput output(ws);
        thread.start(output);

        if (isatty(0))
        {
            std::cout << std::endl;
            std::cout << "Enter LOOL WS requests, one per line. Enter EOF to finish." << std::endl;
        }

        while (!std::cin.eof())
        {
            std::string line;
            std::getline(std::cin, line);
            ws.sendFrame(line.c_str(), line.size());
        }
        thread.join();
        _srv.stopAll();
        _main.terminate();
    }

private:
    ServerApplication& _main;
    ServerSocket& _svs;
    HTTPServer& _srv;
};

class Undertaker : public Runnable
{
public:
    Undertaker()
    {
    }

    void run() override
    {
        bool someChildrenHaveDied = false;

        while (!someChildrenHaveDied || MasterProcessSession::_childProcesses.size() > 0)
        {
            int status;
            pid_t pid = waitpid(-1, &status, 0);
            if (pid < 0)
            {
                if (errno == ECHILD)
                {
                    if (!someChildrenHaveDied)
                    {
                        // We haven't spawned any children yet, or at least none has died yet. So
                        // wait a bit and try again
                        Thread::sleep(1000);
                        continue;
                    }
                    else
                    {
                        // We have spawned children, and we think that we still have them running,
                        // but we don't, huh? Something badly messed up, or just a timing glitch,
                        // like we are at the moment in the process of spawning new children?
                        // Sleep or return from the function (i.e. finish the Undertaker thread)?
                        std::cout << Util::logPrefix() << "No child processes even if we think there should be some!?" << std::endl;
                        return;
                    }
                }
            }

            if (WIFSIGNALED(status))
                Application::instance().logger().error(Util::logPrefix() + "Child " + std::to_string(pid) + " killed by signal " + Util::signalName(WTERMSIG(status)));
            else
                Application::instance().logger().information(Util::logPrefix() + "Child " + std::to_string(pid) + " died normally, status: " + std::to_string(WEXITSTATUS(status)));

            if (MasterProcessSession::_childProcesses.find(pid) == MasterProcessSession::_childProcesses.end())
                std::cout << Util::logPrefix() << "(Not one of our known child processes)" << std::endl;
            else
            {
                File jailDir(MasterProcessSession::getJailPath(MasterProcessSession::_childProcesses[pid]));
                MasterProcessSession::_childProcesses.erase(pid);
                someChildrenHaveDied = true;
                if (!jailDir.exists() || !jailDir.isDirectory())
                {
                    Application::instance().logger().error(Util::logPrefix() + "Jail '" + jailDir.path() + "' does not exist or is not a directory");
                }
                else
                {
                    std::cout << Util::logPrefix() << "Removing jail tree " << jailDir.path() << std::endl;
                    jailDir.remove(true);
                }
            }
        }
        std::cout << Util::logPrefix() << "All child processes have died (I hope)" << std::endl;
    }
};

int LOOLWSD::portNumber = DEFAULT_CLIENT_PORT_NUMBER;
std::string LOOLWSD::sysTemplate;
std::string LOOLWSD::loTemplate;
std::string LOOLWSD::childRoot;
std::string LOOLWSD::loSubPath = "lo";
std::string LOOLWSD::jail;
int LOOLWSD::_numPreSpawnedChildren = 10;
const std::string LOOLWSD::CHILD_URI = "/loolws/child/";

LOOLWSD::LOOLWSD() :
    _doTest(false),
    _childId(0)
{
}

LOOLWSD::~LOOLWSD()
{
}

void LOOLWSD::initialize(Application& self)
{
    ServerApplication::initialize(self);
}

void LOOLWSD::uninitialize()
{
    ServerApplication::uninitialize();
}

void LOOLWSD::defineOptions(OptionSet& options)
{
    ServerApplication::defineOptions(options);

    options.addOption(Option("help", "", "Display help information on command line arguments.")
                      .required(false)
                      .repeatable(false));

    options.addOption(Option("port", "", "Port number to listen to (default: " + std::to_string(DEFAULT_CLIENT_PORT_NUMBER) + "),"
                             " must not be " + std::to_string(MASTER_PORT_NUMBER) + ".")
                      .required(false)
                      .repeatable(false)
                      .argument("port number"));

    options.addOption(Option("systemplate", "", "Path to a template tree with shared libraries etc to be used as source for chroot jails for child processes.")
                      .required(false)
                      .repeatable(false)
                      .argument("directory"));

    options.addOption(Option("lotemplate", "", "Path to a LibreOffice installation tree to be copied (linked) into the jails for child processes. Should be on the same file system as systemplate.")
                      .required(false)
                      .repeatable(false)
                      .argument("directory"));

    options.addOption(Option("childroot", "", "Path to the directory under which the chroot jails for the child processes will be created. Should be on the same file system as systemplate and lotemplate.")
                      .required(false)
                      .repeatable(false)
                      .argument("directory"));

    options.addOption(Option("losubpath", "", "Relative path where the LibreOffice installation will be copied inside a jail (default: '" + loSubPath + "').")
                      .required(false)
                      .repeatable(false)
                      .argument("relative path"));

    options.addOption(Option("numprespawns", "", "Number of child processes to keep started in advance and waiting for new clients.")
                      .required(false)
                      .repeatable(false)
                      .argument("port number"));

    options.addOption(Option("test", "", "Interactive testing.")
                      .required(false)
                      .repeatable(false));

    options.addOption(Option("child", "", "For internal use only.")
                      .required(false)
                      .repeatable(false)
                      .argument("child id"));

    options.addOption(Option("jail", "", "For internal use only.")
                      .required(false)
                      .repeatable(false)
                      .argument("directory"));

#if ENABLE_DEBUG
    options.addOption(Option("uid", "", "Uid to assume if running under sudo for debugging purposes.")
                      .required(false)
                      .repeatable(false)
                      .argument("uid"));
#endif
}

void LOOLWSD::handleOption(const std::string& name, const std::string& value)
{
    ServerApplication::handleOption(name, value);

    if (name == "help")
    {
        displayHelp();
        exit(Application::EXIT_OK);
    }
    else if (name == "port")
        portNumber = std::stoi(value);
    else if (name == "systemplate")
        sysTemplate = value;
    else if (name == "lotemplate")
        loTemplate = value;
    else if (name == "childroot")
        childRoot = value;
    else if (name == "losubpath")
        loSubPath = value;
    else if (name == "numprespawns")
        _numPreSpawnedChildren = std::stoi(value);
    else if (name == "test")
        _doTest = true;
    else if (name == "child")
        _childId = std::stoull(value);
    else if (name == "jail")
        jail = value;
#if ENABLE_DEBUG
    else if (name == "uid")
        uid = std::stoull(value);
#endif
}

void LOOLWSD::displayHelp()
{
    HelpFormatter helpFormatter(options());
    helpFormatter.setCommand(commandName());
    helpFormatter.setUsage("OPTIONS");
    helpFormatter.setHeader("LibreOffice On-Line WebSocket server.");
    helpFormatter.format(std::cout);
}

namespace
{
    void dropChrootCapability()
    {
#ifdef __linux
        cap_t caps;
        cap_value_t cap_list[] = { CAP_SYS_CHROOT };

        caps = cap_get_proc();
        if (caps == NULL)
        {
            Application::instance().logger().error(Util::logPrefix() + "cap_get_proc() failed: " + strerror(errno));
            exit(1);
        }

        if (cap_set_flag(caps, CAP_EFFECTIVE, sizeof(cap_list)/sizeof(cap_list[0]), cap_list, CAP_CLEAR) == -1 ||
            cap_set_flag(caps, CAP_PERMITTED, sizeof(cap_list)/sizeof(cap_list[0]), cap_list, CAP_CLEAR) == -1)
        {
            Application::instance().logger().error(Util::logPrefix() +  "cap_set_flag() failed: " + strerror(errno));
            exit(1);
        }

        if (cap_set_proc(caps) == -1)
        {
            Application::instance().logger().error(std::string("cap_set_proc() failed: ") + strerror(errno));
            exit(1);
        }
        cap_free(caps);
#endif
        if (geteuid() == 0 && getuid() != 0)
        {
            // The program is setuid root. Not normal on Linux where we use setcap, but if this
            // needs to run on non-Linux Unixes, setuid root is what it will bneed to be to be able
            // to do chroot().
            if (setuid(getuid()) != 0) {
                Application::instance().logger().error(std::string("setuid() failed: ") + strerror(errno));
            }
        }
#if ENABLE_DEBUG
        if (geteuid() == 0 && getuid() == 0)
        {
            // Running under sudo, probably because being debugged? Let's drop super-user rights.
            if (uid == 0)
            {
                struct passwd *nobody = getpwnam("nobody");
                if (nobody)
                    uid = nobody->pw_uid;
                else
                    uid = 65534;
            }
            if (setuid(uid) != 0) {
                Application::instance().logger().error(std::string("setuid() failed: ") + strerror(errno));
            }
        }
#endif
    }
}

int LOOLWSD::childMain()
{
    std::cout << Util::logPrefix() << "Child here! id=" << _childId << std::endl;

    // We use the same option set for both parent and child loolwsd,
    // so must check options required in the child (but not in the
    // parent) separately now. And also for options that are
    // meaningless to the child.
    if (jail == "")
        throw MissingOptionException("systemplate");

    if (sysTemplate != "")
        throw IncompatibleOptionsException("systemplate");
    if (loTemplate != "")
        throw IncompatibleOptionsException("lotemplate");
    if (childRoot != "")
        throw IncompatibleOptionsException("childroot");

    if (chroot(jail.c_str()) == -1)
    {
        logger().error("chroot(\"" + jail + "\") failed: " + strerror(errno));
        exit(1);
    }

    dropChrootCapability();

    if (chdir("/") == -1)
    {
        logger().error(std::string("chdir(\"/\") in jail failed: ") + strerror(errno));
        exit(1);
    }

    if (std::getenv("SLEEPFORDEBUGGER"))
    {
        std::cout << "Sleeping " << std::getenv("SLEEPFORDEBUGGER") << " seconds, " <<
            "attach process " << Poco::Process::id() << " in debugger now." << std::endl;
        Thread::sleep(std::stoul(std::getenv("SLEEPFORDEBUGGER")) * 1000);
    }

    try
    {
        LibreOfficeKit *loKit(lok_init_2(("/" + loSubPath + "/program").c_str(), "file:///user"));

        if (!loKit)
        {
            logger().fatal(Util::logPrefix() + "LibreOfficeKit initialisation failed");
            return Application::EXIT_UNAVAILABLE;
        }

        // Open websocket connection between the child process and the
        // parent. The parent forwards us requests that it can't handle.

        HTTPClientSession cs("127.0.0.1", MASTER_PORT_NUMBER);
        cs.setTimeout(0);
        HTTPRequest request(HTTPRequest::HTTP_GET, LOOLWSD::CHILD_URI);
        HTTPResponse response;
        WebSocket ws(cs, request, response);

        ChildProcessSession session(ws, loKit);

        ws.setReceiveTimeout(0);

        std::string hello("child " + std::to_string(_childId));
        session.sendTextFrame(hello);

        int flags;
        int n;
        do
        {
            char buffer[1024];
            n = ws.receiveFrame(buffer, sizeof(buffer), flags);

            if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                if (!session.handleInput(buffer, n))
                    n = 0;
        }
        while (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
    }
    catch (Poco::Exception& exc)
    {
        logger().log(Util::logPrefix() + "Exception: " + exc.what());
    }
    catch (std::exception& exc)
    {
        logger().error(Util::logPrefix() + "Exception: " + exc.what());
    }

    // Safest to just bluntly exit
    _Exit(Application::EXIT_OK);
}

int LOOLWSD::main(const std::vector<std::string>& args)
{
    if (childMode())
        return childMain();

    dropChrootCapability();

    if (access(LOOLWSD_CACHEDIR, R_OK | W_OK | X_OK) != 0)
    {
        std::cout << "Unable to access " << LOOLWSD_CACHEDIR <<
            ", please make sure it exists, and has write permission for this user." << std::endl;
        return Application::EXIT_UNAVAILABLE;
    }

    // We use the same option set for both parent and child loolwsd,
    // so must check options required in the parent (but not in the
    // child) separately now. Also check for options that are
    // meaningless for the parent.
    if (sysTemplate == "")
        throw MissingOptionException("systemplate");
    if (loTemplate == "")
        throw MissingOptionException("lotemplate");
    if (childRoot == "")
        throw MissingOptionException("childroot");

    if (_childId != 0)
        throw IncompatibleOptionsException("child");
    if (jail != "")
        throw IncompatibleOptionsException("jail");
    if (portNumber == MASTER_PORT_NUMBER)
        throw IncompatibleOptionsException("port");


    // Set up a thread to reap child processes and clean up after them
    Undertaker undertaker;
    Thread undertakerThread;
    undertakerThread.start(undertaker);

    // Start a server listening on the port for clients
    ServerSocket svs(portNumber, _numPreSpawnedChildren*10);
    Poco::ThreadPool threadPool(_numPreSpawnedChildren*2, _numPreSpawnedChildren*5);
    HTTPServer srv(new RequestHandlerFactory(), threadPool, svs, new HTTPServerParams);

    srv.start();

    // And one on the port for child processes
    SocketAddress addr2("127.0.0.1", MASTER_PORT_NUMBER);
    ServerSocket svs2(addr2, _numPreSpawnedChildren);
    Poco::ThreadPool threadPool2(_numPreSpawnedChildren*2, _numPreSpawnedChildren*5);
    HTTPServer srv2(new RequestHandlerFactory(), threadPool2, svs2, new HTTPServerParams);

    srv2.start();

    if (_doTest)
        _numPreSpawnedChildren = 1;

    for (int i = 0; i < _numPreSpawnedChildren; i++)
        MasterProcessSession::preSpawn();

    TestInput input(*this, svs, srv);
    Thread inputThread;
    if (_doTest)
    {
        inputThread.start(input);
    }

    waitForTerminationRequest();

    // Doing this causes a crash. So just let the process proceed and exit.
    // srv.stop();

    if (_doTest)
        inputThread.join();

    // Terminate child processes
    for (auto i : MasterProcessSession::_childProcesses)
    {
        logger().information(Util::logPrefix() + "Requesting child process " + std::to_string(i.first) + " to terminate");
        Process::requestTermination(i.first);
    }

    undertakerThread.join();

    return Application::EXIT_OK;
}

bool LOOLWSD::childMode() const
{
    return _childId != 0;
}

POCO_SERVER_MAIN(LOOLWSD)

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
