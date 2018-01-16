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

#include "RemoteToolClient.h"

#include "RemoteToolFrames.h"
#include "ToolBalancer.h"

#include <CoordinatorClient.h>
#include <SocketFrameService.h>
#include <ThreadUtils.h>
#include <FileUtils.h>

#include <stdio.h>
#include <stdlib.h>
#include <functional>
#include <fstream>
#include <algorithm>

namespace Wuild
{
static const size_t g_recommendedBufferSize = 64 * 1024;


class RemoteToolRequestWrap
{
public:
	TimePoint m_start;
	int64_t m_taskIndex = 0;
	ToolInvocation m_invocation;
	ToolInvocation m_originalInvocation;
	RemoteToolRequest::Ptr m_toolRequest;
	RemoteToolClient::InvokeCallback m_callback;
	TimePoint m_expirationMoment;
	TimePoint m_requestTimeout;
	int m_attemptsRemain = 1;
};

class RemoteToolClientImpl
{
public:
	RemoteToolClient * m_parent = nullptr; // ugly..
	IToolInvoker * m_invokerFallback = nullptr;
	ToolBalancer m_balancer;
	std::mutex m_clientsMutex;
	std::deque<SocketFrameHandler::Ptr> m_clients;
	std::mutex m_requestsMutex;
	std::deque<RemoteToolRequestWrap> m_requests;
	std::unique_ptr<SocketFrameService> m_server;
	CoordinatorClient m_coordinator;
	size_t m_clientIndex = 0;
	std::atomic_int m_pendingTasks {0};

	void QueueTask(const RemoteToolRequestWrap & task)
	{
		std::lock_guard<std::mutex> lock(m_requestsMutex);
		m_requests.push_back(task);
		m_pendingTasks++;
	}

	void ProcessTasks()
	{
		RemoteToolRequestWrap task;
		{
			std::lock_guard<std::mutex> lock(m_requestsMutex);
			if (m_requests.empty())
				return;
			TimePoint now(true);
			for (auto it = m_requests.begin(); it != m_requests.end(); )
			{
				RemoteToolRequestWrap & request = *it;
				if (request.m_expirationMoment > now)
				{
					it++;
					continue;
				}

				Syslogger(Syslogger::Err) << "Task expired: " << SocketFrame::Ptr(request.m_toolRequest)
										  << ", expiration moment:" << request.m_expirationMoment.ToString()
										  << ", now:" << now.ToString();
				if (request.m_callback)
				{
					if (m_invokerFallback)
					{
						Syslogger(Syslogger::Info) << "Using invoker fallback.";
						m_invokerFallback->InvokeTool(request.m_originalInvocation, request.m_callback);
					}
					else
					{
						RemoteToolClient::TaskExecutionInfo info;
						info.m_stdOutput = "Timeout expired.";
						request.m_callback(info);
					}
				}
				it = m_requests.erase(it);
			}

			if (m_requests.empty())
				return;

			task = *m_requests.begin();
		}

		size_t clientIndex = m_balancer.FindFreeClient(task.m_invocation.m_id.m_toolId);
		if (clientIndex == std::numeric_limits<size_t>::max())
			return;

		SocketFrameHandler::Ptr handler;
		{
			std::lock_guard<std::mutex> lock2(m_clientsMutex);
			handler = m_clients[clientIndex];
		}
		auto frameCallback = [this, task, clientIndex](SocketFrame::Ptr responseFrame, SocketFrameHandler::ReplyState state, const std::string & errorInfo)
		{
			m_balancer.FinishTask(clientIndex);
			const std::string outputFilename =  task.m_originalInvocation.GetOutput();
			Syslogger(Syslogger::Info) << "RECIEVING [" << task.m_taskIndex << "]:" << outputFilename;
			RemoteToolClient::TaskExecutionInfo info;
			bool retry = false;
			if (state == SocketFrameHandler::ReplyState::Timeout)
			{
				info.m_stdOutput = "Timeout expired:" + outputFilename + ", start:" + task.m_start.ToString()
						+ " exp:" + task.m_expirationMoment.ToString() + ", remain:" + std::to_string(task.m_attemptsRemain)
						+ ", balancer.free:" + std::to_string(m_balancer.GetFreeThreads()) + ", extraInfo:" + errorInfo;
				retry = true;
			}
			else if (state == SocketFrameHandler::ReplyState::Error)
			{
				info.m_stdOutput = "Internal error. " + errorInfo;
				retry = true;
			}
			else
			{
				RemoteToolResponse::Ptr result = std::dynamic_pointer_cast<RemoteToolResponse>(responseFrame);
				info.m_toolExecutionTime = result->m_executionTime;
				info.m_networkRequestTime = task.m_start.GetElapsedTime();

				info.m_result = result->m_result;
				info.m_stdOutput = result->m_stdOut;
				std::replace(info.m_stdOutput.begin(), info.m_stdOutput.end(), '\r', ' ');

				if (info.m_result && !outputFilename.empty())
				{
					info.m_result = FileInfo(outputFilename).WriteCompressed(result->m_fileData, result->m_compression);
				}
			}
			m_parent->UpdateSessionInfo(info);
			if (task.m_attemptsRemain > 0 && retry)
			{
				Syslogger(Syslogger::Warning) << info.m_stdOutput << " Retrying (" << task.m_attemptsRemain << " attempts remain), args:" << task.m_invocation.GetArgsString(false);
				auto taskCopy = task;
				taskCopy.m_attemptsRemain--;
				taskCopy.m_taskIndex = this->m_parent->m_taskIndex++;
				taskCopy.m_expirationMoment = TimePoint(true) + m_parent->m_config.m_queueTimeout;
				this->QueueTask(taskCopy);
			}
			else
			{
				task.m_callback(info);
			}
		};
		m_balancer.StartTask(clientIndex);
		m_pendingTasks--;
		handler->QueueFrame(task.m_toolRequest, frameCallback, task.m_requestTimeout);
		{
			std::lock_guard<std::mutex> lock(m_requestsMutex);
			m_requests.pop_front();
		}
	}
	SocketFrameHandler::Ptr AddClient(const std::string & host, int16_t port);
};


RemoteToolClient::RemoteToolClient(IInvocationRewriter::Ptr invocationRewriter)
	: m_impl(new RemoteToolClientImpl()), m_invocationRewriter(invocationRewriter)
{
	m_impl->m_parent = this;
	m_impl->m_coordinator.SetInfoArrivedCallback([this](const CoordinatorInfo& info){
		for (const auto & client : info.m_toolServers)
			this->AddClient(client, true);
	});
}

RemoteToolClient::~RemoteToolClient()
{
	FinishSession();
	m_thread.Stop();

	for (auto & client : m_impl->m_clients)
		client->Stop();
}

bool RemoteToolClient::SetConfig(const RemoteToolClient::Config &config)
{
	std::ostringstream os;
	if (!config.Validate(&os))
	{
		Syslogger(Syslogger::Err) << os.str();
		return false;
	}
	m_config = config;
	return true;
}

void RemoteToolClient::SetInvokerFallback(IToolInvoker *invokerFallback)
{
	m_impl->m_invokerFallback = invokerFallback;
}

int RemoteToolClient::GetFreeRemoteThreads() const
{
	return static_cast<int>(m_impl->m_balancer.GetFreeThreads()) - m_impl->m_pendingTasks; // may be negative.
}

void RemoteToolClient::Start(const StringVector & requiredToolIds)
{
	m_started = true;
	m_start = m_lastFinish = TimePoint(true);
	m_sessionInfo = ToolServerSessionInfo();
	m_sessionInfo.m_sessionId = m_sessionId = m_start.GetUS();
	m_sessionInfo.m_clientId = m_config.m_clientId;
	m_impl->m_balancer.SetRequiredTools(requiredToolIds);
	m_impl->m_balancer.SetSessionId(m_sessionId);

	for (auto & handler : m_impl->m_clients)
		handler->Start();

	if (!m_impl->m_coordinator.SetConfig(m_config.m_coordinator))
		return;
	m_impl->m_coordinator.Start();

	m_thread.Exec(std::bind(&RemoteToolClientImpl::ProcessTasks, m_impl.get()));
}

void RemoteToolClient::FinishSession()
{
	if (!m_started)
		return;
	m_started = false;
	m_sessionInfo.m_elapsedTime = m_lastFinish - m_start;
	m_impl->m_coordinator.SendToolServerSessionInfo(m_sessionInfo, true);
}

void RemoteToolClient::SetRemoteAvailableCallback(RemoteToolClient::RemoteAvailableCallback callback)
{
	m_remoteAvailableCallback = callback;
}

void RemoteToolClient::AddClient(const ToolServerInfo &info, bool start)
{
	size_t index = 0;
	ToolBalancer & balancer = m_impl->m_balancer;
	auto status = balancer.UpdateClient(info, index);
	if (status == ToolBalancer::ClientStatus::Skipped)
		return;

	AvailableCheck();

	if (status == ToolBalancer::ClientStatus::Updated)
		return;

	auto handler = m_impl->AddClient(info.m_connectionHost, info.m_connectionPort);
	handler->SetChannelNotifier([&balancer, index, this](bool state){
		balancer.SetClientActive(index, state);
		AvailableCheck();
	});
	if (start)
	   handler->Start();

}

SocketFrameHandler::Ptr RemoteToolClientImpl::AddClient(const std::string &host, int16_t port)
{
	Syslogger() << "RemoteToolClient::AddClient " << host  << ":" <<  port;

	SocketFrameHandlerSettings settings;
	settings.m_channelProtocolVersion       = RemoteToolRequest::s_version + RemoteToolResponse::s_version;
	settings.m_recommendedRecieveBufferSize = g_recommendedBufferSize;
	settings.m_recommendedSendBufferSize    = g_recommendedBufferSize;
	settings.m_segmentSize = 8192;

	SocketFrameHandler::Ptr handler(new SocketFrameHandler( settings ));
	handler->RegisterFrameReader(SocketFrameReaderTemplate<RemoteToolResponse>::Create());
	handler->SetTcpChannel(host, port);

	std::lock_guard<std::mutex> lock(m_clientsMutex);
	m_clients.push_back(handler);

	return handler;
}

void RemoteToolClient::InvokeTool(const ToolInvocation & invocation, InvokeCallback callback)
{
	TimePoint start(true);
	const std::string inputFilename  = invocation.GetInput();
	ByteArrayHolder inputData;
	if (!inputFilename.empty() && !FileInfo(inputFilename).ReadCompressed(inputData, m_config.m_compression))
	{
		callback(RemoteToolClient::TaskExecutionInfo("failed to read " + inputFilename));
		return;
	}

	RemoteToolRequest::Ptr toolRequest(new RemoteToolRequest());
	toolRequest->m_invocation = m_invocationRewriter->PrepareRemote(invocation);
	toolRequest->m_fileData = inputData;
	toolRequest->m_compression = m_config.m_compression;
	toolRequest->m_sessionId = m_sessionId;
	toolRequest->m_clientId = m_config.m_clientId;

	RemoteToolRequestWrap wrap;
	wrap.m_start = start;
	wrap.m_toolRequest = toolRequest;
	wrap.m_taskIndex = m_taskIndex++;
	wrap.m_invocation = toolRequest->m_invocation;
	wrap.m_originalInvocation = invocation;
	wrap.m_callback = callback;
	wrap.m_expirationMoment = wrap.m_start + m_config.m_queueTimeout;
	wrap.m_attemptsRemain = m_config.m_invocationAttempts;
	wrap.m_requestTimeout = m_config.m_requestTimeout;

	Syslogger(Syslogger::Info) << "QueueFrame [" << wrap.m_taskIndex << "] -> " << toolRequest->m_invocation.m_id.m_toolId
							   << " " << toolRequest->m_invocation.GetArgsString(false)
						<< ", balancerFree:" <<m_impl->m_balancer.GetFreeThreads()
						<< ", pending:" << m_impl->m_pendingTasks;

	m_impl->QueueTask(wrap);
}

void RemoteToolClient::UpdateSessionInfo(const RemoteToolClient::TaskExecutionInfo &executionResult)
{
	std::lock_guard<std::mutex> lock(m_sessionInfoMutex);
	m_lastFinish = TimePoint(true);
	m_sessionInfo.m_tasksCount ++;
	if (!executionResult.m_result)
		m_sessionInfo.m_failuresCount++;
	m_sessionInfo.m_totalNetworkTime += executionResult.m_networkRequestTime;
	m_sessionInfo.m_totalExecutionTime += executionResult.m_toolExecutionTime;
	m_sessionInfo.m_currentUsedThreads = m_impl->m_balancer.GetUsedThreads();
	m_sessionInfo.m_maxUsedThreads = std::max(m_sessionInfo.m_maxUsedThreads, m_sessionInfo.m_currentUsedThreads);

	m_impl->m_coordinator.SendToolServerSessionInfo(m_sessionInfo, false);
}

void RemoteToolClient::AvailableCheck()
{
	std::lock_guard<std::mutex> lock(m_availableCheckMutex);
	if (!m_remoteIsAvailable && m_impl->m_balancer.IsAllActive() && m_impl->m_balancer.GetFreeThreads() > 0)
	{
		m_remoteIsAvailable = true;
		if (m_remoteAvailableCallback)
			m_remoteAvailableCallback();
		const auto free  = m_impl->m_balancer.GetFreeThreads();
		const auto total = m_impl->m_balancer.GetTotalThreads();
		Syslogger(Syslogger::Notice) << "Recieved info from coordinator: total remote threads=" << total << ", free=" << free;
	}
}

}
