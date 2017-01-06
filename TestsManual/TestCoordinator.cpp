/*
 * Copyright (C) 2017 Smirnov Vladimir mapron1@gmail.com
 * Source code licensed under the Apache License, Version 2.0 (the "License");
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0 or in file COPYING-APACHE-2.0.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.h
 */

#include "TestUtils.h"

#include <CoordinatorServer.h>
#include <RemoteToolServer.h>
#include <RemoteToolClient.h>

const std::string g_testTool = "testTool", g_testTool2 = "testTool2";
using namespace Wuild;

class LocalExecutorTest : public ILocalExecutor
{
public:
    void AddTask(LocalExecutorTask::Ptr task) override
    {
        using namespace Wuild;
        LocalExecutorResult::Ptr res(new LocalExecutorResult("Stub output OK", true));
        res->m_executionTime.SetUS(1000);
        Syslogger(LOG_INFO) << "AddTask ";
        task->m_callback(res);
    }
    TaskPair SplitTask(LocalExecutorTask::Ptr , std::string & ) override
    {
        return TaskPair();
    }
    virtual StringVector GetToolIds() const  override
    {
        return StringVector(1, g_testTool);
    }
    void SetWorkersCount(int) override {}
};

const int g_workerTestPort = 12345;
const int g_coordinatorTestPort = 12346;

int main(int argc, char** argv)
{    
    ConfiguredApplication app(argc, argv, "TestCoordinator");
    if (!CreateCompiler(app, true))
       return 1;

    app.m_loggerConfig.m_outputTimeoffsets = true;
   // app.m_loggerConfig.m_maxLogLevel = LOG_DEBUG;
    app.InitLogging(app.m_loggerConfig);

    ILocalExecutor::Ptr executor(new LocalExecutorTest());

    CoordinatorClient::Config coordClientConfig;
    coordClientConfig.m_coordinatorHost = "localhost";
    coordClientConfig.m_coordinatorPort = g_coordinatorTestPort;
    coordClientConfig.m_sendWorkerInterval = TimePoint(1.0);
    coordClientConfig.m_logContext = "coordinator:worker";

    CoordinatorServer::Config coordServerConfig;
    coordServerConfig.m_listenPort = g_coordinatorTestPort;

    RemoteToolServer::Config workerConfig;
    workerConfig.m_coordinator = coordClientConfig;
    workerConfig.m_listenHost = "localhost";
    workerConfig.m_listenPort = g_workerTestPort;

    coordClientConfig.m_logContext = "coordinator:client";
    coordClientConfig.m_sendWorkerInterval = TimePoint(false);

    RemoteToolClient::Config clientConfig;
    clientConfig.m_coordinator = coordClientConfig;
    clientConfig.m_invocationAttempts = 1;
    clientConfig.m_minimalRemoteTasks = 1;
    clientConfig.m_queueTimeout = TimePoint(1.0);

    RemoteToolServer rcServer(executor);
    if (!rcServer.SetConfig(workerConfig))
        return 1;

    RemoteToolClient rcClient;
    if (!rcClient.SetConfig(clientConfig))
        return 1;

    CoordinatorServer coordServer;
    if (!coordServer.SetConfig(coordServerConfig))
        return 1;

    rcServer.Start();
    rcClient.Start();
    coordServer.Start();

    std::atomic_int totalFinished {0}, totalCount {0};
    auto callback = [&totalFinished, &totalCount]( const RemoteToolClient::ExecutionInfo& info){
        if (info.m_stdOutput.size())
            std::cout << info.m_stdOutput << std::endl << std::flush;

        std::cout << info.GetProfilingStr() << " \n";
        totalFinished++;
        if (totalFinished == totalCount)
           Application::Interrupt(1 - info.m_result);
    };
    auto callbackFail = [&totalFinished, &totalCount]( const RemoteToolClient::ExecutionInfo& info){
        if (info.m_stdOutput.size())
            std::cout << info.m_stdOutput << std::endl << std::flush;

        totalFinished++;
        if (totalFinished == totalCount)
           Application::Interrupt(0);
    };

    TimePoint start(true);
    rcClient.SetRemoteAvailableCallback([start](int ) {
         std::cout <<  "Init client taken: " << start.GetElapsedTime().GetUS() << " us." << std::endl << std::flush;
    });
    totalCount++; rcClient.InvokeTool(CompilerInvocation().SetId(g_testTool) , callback);
    totalCount++; rcClient.InvokeTool(CompilerInvocation().SetId(g_testTool2), callbackFail);

    return ExecAppLoop(TestConfiguration::ExitHandler);
}